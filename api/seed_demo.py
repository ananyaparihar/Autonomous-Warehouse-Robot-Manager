"""Fill a running API with demo data and walk one trip through its whole life.

1. In one terminal (api folder):   py -m uvicorn main:app --reload
2. In another terminal:            py seed_demo.py

Uses only the standard library. Safe to run on an empty database; on a database that
already has these SKUs it reports which request was refused and stops.
"""

import json
import sys
import urllib.error
import urllib.request

BASE = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else "http://127.0.0.1:8000"


def call(method: str, path: str, body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(BASE + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as err:
        detail = err.read().decode("utf-8", errors="replace")
        raise SystemExit(f"{method} {path} was refused ({err.code}): {detail}")
    except urllib.error.URLError as err:
        raise SystemExit(f"Cannot reach the API at {BASE}: {err.reason}\nIs 'py -m uvicorn main:app' running?")


def main() -> None:
    print("health:", call("GET", "/health"))

    # The 8-node floor used in the C++ tests: node 0 is the depot, robot carries up to 50.
    edges = [(0, 1, 4), (1, 2, 3), (2, 3, 5), (0, 4, 2), (4, 5, 6),
             (5, 6, 2), (6, 7, 4), (1, 5, 3), (3, 7, 6), (2, 6, 4)]
    call("PUT", "/floor", {"nodes": 8, "depot": 0, "capacity": 50,
                           "edges": [{"u": u, "v": v, "length": w} for u, v, w in edges]})
    print("floor configured")

    products = [  # sku, name, shelf node, stock, unit weight, unit value
        (101, "Cable organiser", 3, 20, 10, 60),
        (102, "Desk lamp", 6, 15, 20, 100),
        (103, "Monitor stand", 7, 8, 30, 120),
        (104, "USB hub", 1, 40, 2, 25),
    ]
    for sku, name, node, qty, weight, value in products:
        call("POST", "/products", {"sku": sku, "name": name, "node": node, "quantity": qty,
                                   "unit_weight": weight, "unit_value": value})
    print(f"{len(products)} products created")

    orders = [(101, 1, 3), (102, 1, 2), (103, 1, 1), (104, 3, 4)]   # sku, quantity, urgency
    for sku, qty, urgency in orders:
        call("POST", "/orders", {"sku": sku, "quantity": qty, "urgency": urgency})
    print(f"{len(orders)} orders created")

    plan = call("POST", "/trips/plan")
    trip = plan["trip"]
    print("\n" + plan["message"])
    print("  orders on trip :", trip["order_ids"])
    print("  left for later :", plan["deferred"])
    print("  stops          :", [(s["node"], s["orders"]) for s in trip["stops"]])
    print("  route          :", " -> ".join(map(str, trip["route"])))

    done = call("POST", f"/trips/{trip['id']}/complete")
    print(f"\ntrip {done['id']} completed. Stock after picking:")
    for p in call("GET", "/products"):
        print(f"  SKU {p['sku']} {p['name']:<16} on shelf {p['quantity']:>3}  reserved {p['reserved']}  available {p['available']}")
    print("\nsummary:", call("GET", "/stats"))
    print(f"\nExplore the rest at {BASE}/docs")


if __name__ == "__main__":
    main()
