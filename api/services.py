"""Business rules of the warehouse. The routes in main.py are thin wrappers around these.

Every function that changes data does it inside transaction(), so a failure
halfway (a rule violated, the engine down) leaves the database exactly as it was.

Order life cycle
    pending --plan--> planned --complete trip--> completed
       |                 |
       |                 +--cancel trip--> pending
       +--cancel--> cancelled           (the engine can also mark an order rejected)

Stock rule: units promised to pending or planned orders are "reserved".
available = quantity - reserved, and an order may only be created if it fits
in what is available. Stock only changes through restock, adjust and
completing a trip, and each change is written to stock_movements.
"""

import json
import sqlite3
from typing import Any, Dict, List, Optional

from fastapi import HTTPException

import engine_client
from database import now, transaction

OPEN_STATUSES = ("pending", "planned")


def conflict(message: str) -> HTTPException:
    return HTTPException(status_code=409, detail=message)


def not_found(what: str) -> HTTPException:
    return HTTPException(status_code=404, detail=f"{what} not found")


def invalid(message: str) -> HTTPException:
    return HTTPException(status_code=422, detail=message)


# =====================================================================
# floor
# =====================================================================

def get_floor(conn: sqlite3.Connection) -> Optional[Dict[str, Any]]:
    row = conn.execute("SELECT nodes, depot, capacity FROM floor WHERE id = 1").fetchone()
    if row is None:
        return None
    edges = [
        {"u": r["u"], "v": r["v"], "length": r["length"]}
        for r in conn.execute("SELECT u, v, length FROM floor_edges ORDER BY id")
    ]
    return {"nodes": row["nodes"], "depot": row["depot"], "capacity": row["capacity"], "edges": edges}


def require_floor(conn: sqlite3.Connection) -> Dict[str, Any]:
    floor = get_floor(conn)
    if floor is None:
        raise conflict("the floor is not configured yet: send PUT /floor first")
    return floor


def set_floor(conn: sqlite3.Connection, data: Dict[str, Any]) -> Dict[str, Any]:
    nodes = data["nodes"]
    if data["depot"] >= nodes:
        raise invalid("depot must be one of the floor nodes (0 to nodes-1)")
    for e in data["edges"]:
        if e["u"] >= nodes or e["v"] >= nodes:
            raise invalid(f"edge {e['u']}-{e['v']} refers to a node outside 0..{nodes - 1}")

    with transaction(conn):
        if conn.execute("SELECT 1 FROM trips WHERE status = 'planned' LIMIT 1").fetchone():
            raise conflict("there are planned trips: complete or cancel them before changing the floor")
        stray = conn.execute("SELECT sku, node FROM products WHERE node >= ? LIMIT 1", (nodes,)).fetchone()
        if stray:
            raise conflict(f"product {stray['sku']} sits on node {stray['node']}, outside the new floor")
        conn.execute("DELETE FROM floor")
        conn.execute("DELETE FROM floor_edges")
        conn.execute(
            "INSERT INTO floor (id, nodes, depot, capacity) VALUES (1, ?, ?, ?)",
            (nodes, data["depot"], data["capacity"]),
        )
        conn.executemany(
            "INSERT INTO floor_edges (u, v, length) VALUES (?, ?, ?)",
            [(e["u"], e["v"], e["length"]) for e in data["edges"]],
        )
    return get_floor(conn)  # type: ignore[return-value]


def _floor_for_engine(floor: Dict[str, Any]) -> Dict[str, Any]:
    return {"nodes": floor["nodes"], "edges": [[e["u"], e["v"], e["length"]] for e in floor["edges"]]}


def shortest_path(conn: sqlite3.Connection, source: int, target: int, engine: Dict[str, Any]) -> Dict[str, Any]:
    floor = require_floor(conn)
    for n in (source, target):
        if n < 0 or n >= floor["nodes"]:
            raise invalid(f"node {n} is not on the floor (0 to {floor['nodes'] - 1})")
    r = engine_client.shortest_path(_floor_for_engine(floor), source, target, **engine)
    return {"reachable": r["reachable"], "distance": r["distance"], "path": r["path"]}


def network(conn: sqlite3.Connection, algorithm: str, engine: Dict[str, Any]) -> Dict[str, Any]:
    floor = require_floor(conn)
    r = engine_client.network_mst(_floor_for_engine(floor), algorithm, **engine)
    return {
        "algorithm": r["algorithm"], "spanning": r["spanning"], "components": r["components"],
        "total_weight": r["total_weight"], "edges": r["edges"],
    }


# =====================================================================
# products and stock
# =====================================================================

_PRODUCT_SQL = """
    SELECT p.sku, p.name, p.node, p.quantity, p.unit_weight, p.unit_value,
           COALESCE(r.reserved, 0) AS reserved
    FROM products p
    LEFT JOIN (SELECT sku, SUM(quantity) AS reserved FROM orders
               WHERE status IN ('pending', 'planned') GROUP BY sku) r ON r.sku = p.sku
"""


def _product(row: sqlite3.Row) -> Dict[str, Any]:
    d = dict(row)
    d["available"] = d["quantity"] - d["reserved"]
    return d


def get_product(conn: sqlite3.Connection, sku: int) -> Dict[str, Any]:
    row = conn.execute(_PRODUCT_SQL + " WHERE p.sku = ?", (sku,)).fetchone()
    if row is None:
        raise not_found(f"product {sku}")
    return _product(row)


def list_products(conn: sqlite3.Connection, search: Optional[str], limit: int, offset: int) -> List[Dict[str, Any]]:
    sql, args = _PRODUCT_SQL, []
    if search:
        escaped = search.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
        sql += " WHERE p.name LIKE ? ESCAPE '\\'"
        args.append(f"%{escaped}%")
    sql += " ORDER BY p.sku LIMIT ? OFFSET ?"
    args += [limit, offset]
    return [_product(r) for r in conn.execute(sql, args)]


def _record_movement(conn: sqlite3.Connection, sku: int, delta: int, reason: str, order_id: Optional[int] = None) -> None:
    conn.execute(
        "INSERT INTO stock_movements (sku, delta, reason, order_id, created_at) VALUES (?, ?, ?, ?, ?)",
        (sku, delta, reason, order_id, now()),
    )


def _reserved(conn: sqlite3.Connection, sku: int) -> int:
    row = conn.execute(
        "SELECT COALESCE(SUM(quantity), 0) AS n FROM orders WHERE sku = ? AND status IN ('pending', 'planned')",
        (sku,),
    ).fetchone()
    return int(row["n"])


def create_product(conn: sqlite3.Connection, data: Dict[str, Any]) -> Dict[str, Any]:
    floor = require_floor(conn)
    if data["node"] >= floor["nodes"]:
        raise invalid(f"node {data['node']} is not on the floor (0 to {floor['nodes'] - 1})")
    with transaction(conn):
        if conn.execute("SELECT 1 FROM products WHERE sku = ?", (data["sku"],)).fetchone():
            raise conflict(f"product {data['sku']} already exists")
        conn.execute(
            "INSERT INTO products (sku, name, node, quantity, unit_weight, unit_value) VALUES (?, ?, ?, ?, ?, ?)",
            (data["sku"], data["name"], data["node"], data["quantity"], data["unit_weight"], data["unit_value"]),
        )
        if data["quantity"] > 0:
            _record_movement(conn, data["sku"], data["quantity"], "initial stock")
    return get_product(conn, data["sku"])


def patch_product(conn: sqlite3.Connection, sku: int, data: Dict[str, Any]) -> Dict[str, Any]:
    changes = {k: v for k, v in data.items() if v is not None}
    with transaction(conn):
        if conn.execute("SELECT 1 FROM products WHERE sku = ?", (sku,)).fetchone() is None:
            raise not_found(f"product {sku}")
        affects_plans = any(k in changes for k in ("node", "unit_weight", "unit_value"))
        if affects_plans and conn.execute(
            "SELECT 1 FROM orders WHERE sku = ? AND status = 'planned' LIMIT 1", (sku,)
        ).fetchone():
            raise conflict("this product is in a planned trip: complete or cancel the trip before changing it")
        if "node" in changes:
            floor = require_floor(conn)
            if changes["node"] >= floor["nodes"]:
                raise invalid(f"node {changes['node']} is not on the floor (0 to {floor['nodes'] - 1})")
        if changes:
            assignments = ", ".join(f"{col} = ?" for col in changes)   # column names come from our own schema, not the client
            conn.execute(f"UPDATE products SET {assignments} WHERE sku = ?", list(changes.values()) + [sku])
    return get_product(conn, sku)


def delete_product(conn: sqlite3.Connection, sku: int) -> None:
    with transaction(conn):
        if conn.execute("SELECT 1 FROM products WHERE sku = ?", (sku,)).fetchone() is None:
            raise not_found(f"product {sku}")
        if conn.execute("SELECT 1 FROM orders WHERE sku = ? LIMIT 1", (sku,)).fetchone():
            raise conflict("this product has orders and cannot be deleted")
        conn.execute("DELETE FROM products WHERE sku = ?", (sku,))


def restock(conn: sqlite3.Connection, sku: int, quantity: int) -> Dict[str, Any]:
    with transaction(conn):
        if conn.execute("SELECT 1 FROM products WHERE sku = ?", (sku,)).fetchone() is None:
            raise not_found(f"product {sku}")
        conn.execute("UPDATE products SET quantity = quantity + ? WHERE sku = ?", (quantity, sku))
        _record_movement(conn, sku, quantity, "restock")
    return get_product(conn, sku)


def adjust_stock(conn: sqlite3.Connection, sku: int, delta: int, reason: str) -> Dict[str, Any]:
    if delta == 0:
        raise invalid("delta cannot be zero")
    with transaction(conn):
        row = conn.execute("SELECT quantity FROM products WHERE sku = ?", (sku,)).fetchone()
        if row is None:
            raise not_found(f"product {sku}")
        new_quantity = row["quantity"] + delta
        if new_quantity < 0:
            raise conflict(f"stock cannot go below zero (currently {row['quantity']})")
        reserved = _reserved(conn, sku)
        if new_quantity < reserved:
            raise conflict(f"{reserved} units are reserved for open orders: stock cannot drop below that")
        conn.execute("UPDATE products SET quantity = ? WHERE sku = ?", (new_quantity, sku))
        _record_movement(conn, sku, delta, f"adjustment: {reason}")
    return get_product(conn, sku)


def list_movements(conn: sqlite3.Connection, sku: Optional[int], limit: int, offset: int) -> List[Dict[str, Any]]:
    sql, args = "SELECT * FROM stock_movements", []
    if sku is not None:
        sql += " WHERE sku = ?"
        args.append(sku)
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?"
    args += [limit, offset]
    return [dict(r) for r in conn.execute(sql, args)]


# =====================================================================
# orders
# =====================================================================

_ORDER_SQL = """
    SELECT o.id, o.sku, o.quantity, o.urgency, o.status, o.trip_id, o.note, o.created_at,
           p.unit_weight * o.quantity AS weight, p.unit_value * o.quantity AS value
    FROM orders o JOIN products p ON p.sku = o.sku
"""


def get_order(conn: sqlite3.Connection, order_id: int) -> Dict[str, Any]:
    row = conn.execute(_ORDER_SQL + " WHERE o.id = ?", (order_id,)).fetchone()
    if row is None:
        raise not_found(f"order {order_id}")
    return dict(row)


def list_orders(conn: sqlite3.Connection, status: Optional[str], sku: Optional[int],
                limit: int, offset: int) -> List[Dict[str, Any]]:
    clauses, args = [], []
    if status:
        clauses.append("o.status = ?")
        args.append(status)
    if sku is not None:
        clauses.append("o.sku = ?")
        args.append(sku)
    sql = _ORDER_SQL + (" WHERE " + " AND ".join(clauses) if clauses else "") + " ORDER BY o.id LIMIT ? OFFSET ?"
    return [dict(r) for r in conn.execute(sql, args + [limit, offset])]


def create_order(conn: sqlite3.Connection, data: Dict[str, Any]) -> Dict[str, Any]:
    floor = require_floor(conn)
    with transaction(conn):
        product = conn.execute(
            "SELECT quantity, unit_weight FROM products WHERE sku = ?", (data["sku"],)
        ).fetchone()
        if product is None:
            raise not_found(f"product {data['sku']}")
        available = product["quantity"] - _reserved(conn, data["sku"])
        if data["quantity"] > available:
            raise conflict(f"only {available} units available (requested {data['quantity']})")
        weight = product["unit_weight"] * data["quantity"]
        if weight > floor["capacity"]:
            raise conflict(
                f"this order weighs {weight} but the robot carries at most {floor['capacity']}: split it into smaller orders"
            )
        cur = conn.execute(
            "INSERT INTO orders (sku, quantity, urgency, status, created_at) VALUES (?, ?, ?, 'pending', ?)",
            (data["sku"], data["quantity"], data["urgency"], now()),
        )
        order_id = cur.lastrowid
    return get_order(conn, order_id)  # type: ignore[arg-type]


def cancel_order(conn: sqlite3.Connection, order_id: int) -> Dict[str, Any]:
    with transaction(conn):
        row = conn.execute("SELECT status FROM orders WHERE id = ?", (order_id,)).fetchone()
        if row is None:
            raise not_found(f"order {order_id}")
        if row["status"] == "planned":
            raise conflict("this order is part of a planned trip: cancel the trip first")
        if row["status"] != "pending":
            raise conflict(f"a {row['status']} order cannot be cancelled")
        conn.execute("UPDATE orders SET status = 'cancelled' WHERE id = ?", (order_id,))
    return get_order(conn, order_id)


# =====================================================================
# trips
# =====================================================================

def _trip(conn: sqlite3.Connection, row: sqlite3.Row) -> Dict[str, Any]:
    ids = [r["id"] for r in conn.execute("SELECT id FROM orders WHERE trip_id = ? ORDER BY id", (row["id"],))]
    return {
        "id": row["id"], "status": row["status"], "order_ids": ids,
        "stops": json.loads(row["stops"]), "route": json.loads(row["route"]),
        "distance": row["distance"], "total_value": row["total_value"], "total_weight": row["total_weight"],
        "stats": json.loads(row["stats"]), "created_at": row["created_at"], "finished_at": row["finished_at"],
    }


def get_trip(conn: sqlite3.Connection, trip_id: int) -> Dict[str, Any]:
    row = conn.execute("SELECT * FROM trips WHERE id = ?", (trip_id,)).fetchone()
    if row is None:
        raise not_found(f"trip {trip_id}")
    return _trip(conn, row)


def list_trips(conn: sqlite3.Connection, status: Optional[str], limit: int, offset: int) -> List[Dict[str, Any]]:
    sql, args = "SELECT * FROM trips", []
    if status:
        sql += " WHERE status = ?"
        args.append(status)
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?"
    args += [limit, offset]
    return [_trip(conn, r) for r in conn.execute(sql, args)]


def plan_trip(conn: sqlite3.Connection, max_candidates: int, engine: Dict[str, Any]) -> Dict[str, Any]:
    """Ask the C++ engine to plan one trip from the pending orders, then record the result."""
    with transaction(conn):
        floor = require_floor(conn)
        rows = conn.execute(
            """SELECT o.id, o.sku, o.quantity, o.urgency, p.node, p.unit_weight, p.unit_value
               FROM orders o JOIN products p ON p.sku = o.sku
               WHERE o.status = 'pending' ORDER BY o.id""").fetchall()
        if not rows:
            raise conflict("there are no pending orders to plan")

        # Only the SKUs that pending orders need are sent to the engine.
        inventory, seen = [], set()
        for r in rows:
            if r["sku"] not in seen:
                seen.add(r["sku"])
                inventory.append({"sku": r["sku"], "node": r["node"]})
        orders = [
            {"id": r["id"], "sku": r["sku"], "weight": r["unit_weight"] * r["quantity"],
             "value": r["unit_value"] * r["quantity"], "urgency": r["urgency"]}
            for r in rows                                    # oldest first: ties in urgency are first come, first served
        ]

        result = engine_client.plan_trip(
            floor=_floor_for_engine(floor), capacity=floor["capacity"], inventory=inventory,
            orders=orders, depot=floor["depot"], max_candidates=max_candidates, **engine,
        )

        selected = result["selected"]
        rejected = [{"order_id": r["id"], "reason": r["reason"]} for r in result["rejected"]]
        for r in rejected:
            conn.execute("UPDATE orders SET status = 'rejected', note = ? WHERE id = ?", (r["reason"], r["order_id"]))

        trip = None
        if selected:
            stats = dict(result["stats"])
            stats["elapsed_ms"] = result["elapsed_ms"]
            cur = conn.execute(
                """INSERT INTO trips (status, route, stops, distance, total_value, total_weight, stats, created_at)
                   VALUES ('planned', ?, ?, ?, ?, ?, ?, ?)""",
                (json.dumps(result["route"]), json.dumps(result["stops"]), result["distance"],
                 result["total_value"], result["total_weight"], json.dumps(stats), now()),
            )
            trip_id = cur.lastrowid
            marks = ",".join("?" * len(selected))
            conn.execute(f"UPDATE orders SET status = 'planned', trip_id = ? WHERE id IN ({marks})", [trip_id] + selected)
            trip = get_trip(conn, trip_id)  # type: ignore[arg-type]

    if trip:
        message = (f"trip {trip['id']} planned: {len(selected)} orders, "
                   f"distance {trip['distance']}, value {trip['total_value']}")
    else:
        message = "no trip planned: every pending order was rejected"
    return {"trip": trip, "deferred": result["deferred"], "rejected": rejected, "message": message}


def plan_fleet(conn: sqlite3.Connection, robots: int, max_candidates: int, engine: Dict[str, Any]) -> Dict[str, Any]:
    """Plan one trip per robot from the pending orders: the most urgent orders go to robot 1 first."""
    with transaction(conn):
        floor = require_floor(conn)
        rows = conn.execute(
            """SELECT o.id, o.sku, o.quantity, o.urgency, p.node, p.unit_weight, p.unit_value
               FROM orders o JOIN products p ON p.sku = o.sku
               WHERE o.status = 'pending' ORDER BY o.id""").fetchall()
        if not rows:
            raise conflict("there are no pending orders to plan")

        inventory, seen = [], set()
        for r in rows:
            if r["sku"] not in seen:
                seen.add(r["sku"])
                inventory.append({"sku": r["sku"], "node": r["node"]})
        orders = [
            {"id": r["id"], "sku": r["sku"], "weight": r["unit_weight"] * r["quantity"],
             "value": r["unit_value"] * r["quantity"], "urgency": r["urgency"]}
            for r in rows
        ]

        result = engine_client.plan_fleet(
            floor=_floor_for_engine(floor), capacity=floor["capacity"], inventory=inventory,
            orders=orders, robots=robots, depot=floor["depot"], max_candidates=max_candidates, **engine,
        )

        # orders the engine can never serve are the same for every robot, so read them once
        rejected = [{"order_id": r["id"], "reason": r["reason"]} for r in result["trips"][0]["rejected"]]
        for r in rejected:
            conn.execute("UPDATE orders SET status = 'rejected', note = ? WHERE id = ?", (r["reason"], r["order_id"]))

        trips = []
        for t in result["trips"]:
            if not t["selected"]:
                continue                                     # this robot has nothing to do
            stats = dict(t["stats"]); stats["robot"] = t["robot"]; stats["elapsed_ms"] = result["elapsed_ms"]
            cur = conn.execute(
                """INSERT INTO trips (status, route, stops, distance, total_value, total_weight, stats, created_at)
                   VALUES ('planned', ?, ?, ?, ?, ?, ?, ?)""",
                (json.dumps(t["route"]), json.dumps(t["stops"]), t["distance"],
                 t["total_value"], t["total_weight"], json.dumps(stats), now()),
            )
            trip_id = cur.lastrowid
            marks = ",".join("?" * len(t["selected"]))
            conn.execute(f"UPDATE orders SET status = 'planned', trip_id = ? WHERE id IN ({marks})",
                         [trip_id] + t["selected"])
            trips.append(get_trip(conn, trip_id))  # type: ignore[arg-type]

        deferred = result["trips"][-1]["deferred"]           # still waiting after every robot got a trip

    if trips:
        message = f"{len(trips)} trips planned for {robots} robots"
    else:
        message = "no trip planned: every pending order was rejected"
    return {"trips": trips, "deferred": deferred, "rejected": rejected, "message": message}

def delete_trip(conn: sqlite3.Connection, trip_id: int) -> None:
    """Remove a cancelled trip from the list. Planned and completed trips are kept."""
    with transaction(conn):
        trip = conn.execute("SELECT status FROM trips WHERE id = ?", (trip_id,)).fetchone()
        if trip is None:
            raise not_found(f"trip {trip_id}")
        if trip["status"] != "cancelled":
            raise conflict(f"only cancelled trips can be deleted (this one is {trip['status']})")
        conn.execute("UPDATE orders SET trip_id = NULL WHERE trip_id = ?", (trip_id,))
        conn.execute("DELETE FROM trips WHERE id = ?", (trip_id,))

def complete_trip(conn: sqlite3.Connection, trip_id: int) -> Dict[str, Any]:
    """The robot finished the trip: take the goods out of stock and close the orders."""
    with transaction(conn):
        trip = conn.execute("SELECT status FROM trips WHERE id = ?", (trip_id,)).fetchone()
        if trip is None:
            raise not_found(f"trip {trip_id}")
        if trip["status"] != "planned":
            raise conflict(f"a {trip['status']} trip cannot be completed")
        orders = conn.execute("SELECT id, sku, quantity FROM orders WHERE trip_id = ? ORDER BY id", (trip_id,)).fetchall()
        for o in orders:
            stock = conn.execute("SELECT quantity FROM products WHERE sku = ?", (o["sku"],)).fetchone()
            if stock is None or stock["quantity"] < o["quantity"]:
                have = 0 if stock is None else stock["quantity"]
                raise conflict(f"not enough stock of SKU {o['sku']} for order {o['id']} (needs {o['quantity']}, have {have})")
            conn.execute("UPDATE products SET quantity = quantity - ? WHERE sku = ?", (o["quantity"], o["sku"]))
            _record_movement(conn, o["sku"], -o["quantity"], "pick", o["id"])
            conn.execute("UPDATE orders SET status = 'completed' WHERE id = ?", (o["id"],))
        conn.execute("UPDATE trips SET status = 'completed', finished_at = ? WHERE id = ?", (now(), trip_id))
    return get_trip(conn, trip_id)


def cancel_trip(conn: sqlite3.Connection, trip_id: int) -> Dict[str, Any]:
    """Abandon a planned trip: its orders go back to pending, to be planned again."""
    with transaction(conn):
        trip = conn.execute("SELECT status FROM trips WHERE id = ?", (trip_id,)).fetchone()
        if trip is None:
            raise not_found(f"trip {trip_id}")
        if trip["status"] != "planned":
            raise conflict(f"a {trip['status']} trip cannot be cancelled")
        conn.execute("UPDATE orders SET status = 'pending', trip_id = NULL WHERE trip_id = ?", (trip_id,))
        conn.execute("UPDATE trips SET status = 'cancelled', finished_at = ? WHERE id = ?", (now(), trip_id))
    return get_trip(conn, trip_id)


# =====================================================================
# summary
# =====================================================================

def stats(conn: sqlite3.Connection) -> Dict[str, Any]:
    prod = conn.execute("SELECT COUNT(*) AS n, COALESCE(SUM(quantity), 0) AS units FROM products").fetchone()
    orders = {s: 0 for s in ("pending", "planned", "completed", "cancelled", "rejected")}
    for r in conn.execute("SELECT status, COUNT(*) AS n FROM orders GROUP BY status"):
        orders[r["status"]] = r["n"]
    trips = {s: 0 for s in ("planned", "completed", "cancelled")}
    for r in conn.execute("SELECT status, COUNT(*) AS n FROM trips GROUP BY status"):
        trips[r["status"]] = r["n"]
    return {"products": prod["n"], "units_in_stock": prod["units"], "orders": orders, "trips": trips}
