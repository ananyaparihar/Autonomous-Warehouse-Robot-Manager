"""Tests for the web API, using the real SQLite database and the real C++ engine.

Run (from the api folder, after building the engine):
    py -m pip install fastapi uvicorn httpx
    py -m unittest test_api -v

Each test gets a fresh temporary database. Skipped if the engine is not built.
"""

import os
import sqlite3
import tempfile
import unittest

from fastapi.testclient import TestClient

from engine_client import default_engine_path
from main import create_app

ENGINE = os.environ.get("WAREHOUSE_ENGINE") or str(default_engine_path())
HAVE_ENGINE = os.path.isfile(ENGINE)

# The 8-node floor used throughout the C++ tests. Node 0 is the depot.
FLOOR = {
    "nodes": 8, "depot": 0, "capacity": 50,
    "edges": [{"u": u, "v": v, "length": w} for u, v, w in
              [(0, 1, 4), (1, 2, 3), (2, 3, 5), (0, 4, 2), (4, 5, 6),
               (5, 6, 2), (6, 7, 4), (1, 5, 3), (3, 7, 6), (2, 6, 4)]],
}


@unittest.skipUnless(HAVE_ENGINE, "engine program not built (see engine/README.md)")
class ApiTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tmp.name, "test.db")
        self.client = TestClient(create_app(db_path=self.db_path, engine_path=ENGINE))
        self.client.__enter__()                    # runs startup: creates the tables

    def tearDown(self):
        self.client.__exit__(None, None, None)
        self.tmp.cleanup()

    # ----- helpers -----
    def setup_floor(self, floor=None):
        r = self.client.put("/floor", json=floor or FLOOR)
        self.assertEqual(r.status_code, 200, r.text)

    def add_product(self, sku, node, unit_weight, unit_value, quantity=5, name=None):
        r = self.client.post("/products", json={
            "sku": sku, "name": name or f"item {sku}", "node": node,
            "quantity": quantity, "unit_weight": unit_weight, "unit_value": unit_value})
        self.assertEqual(r.status_code, 201, r.text)
        return r.json()

    def add_order(self, sku, quantity=1, urgency=0):
        r = self.client.post("/orders", json={"sku": sku, "quantity": quantity, "urgency": urgency})
        self.assertEqual(r.status_code, 201, r.text)
        return r.json()

    def classic_setup(self):
        """Three products and three orders that reproduce the knapsack example from the engine tests.

        Order ids 1, 2, 3 weigh 10, 20, 30 and are worth 60, 100, 120 (urgency 3, 2, 1). Payload limit 50.
        """
        self.setup_floor()
        self.add_product(101, node=3, unit_weight=10, unit_value=60)
        self.add_product(102, node=6, unit_weight=20, unit_value=100)
        self.add_product(103, node=7, unit_weight=30, unit_value=120)
        self.add_order(101, urgency=3)
        self.add_order(102, urgency=2)
        self.add_order(103, urgency=1)

    def sql(self, query, args=()):
        conn = sqlite3.connect(self.db_path)
        try:
            conn.execute(query, args)
            conn.commit()
        finally:
            conn.close()


class HealthAndFloor(ApiTestCase):
    def test_health_reports_database_and_engine(self):
        r = self.client.get("/health")
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json()["status"], "ok")
        self.assertEqual(r.json()["engine"], "1.0")

    def test_floor_must_be_configured_first(self):
        self.assertEqual(self.client.get("/floor").status_code, 409)
        r = self.client.post("/products", json={"sku": 1, "name": "x", "node": 0, "unit_weight": 1, "unit_value": 1})
        self.assertEqual(r.status_code, 409)
        self.assertIn("floor", r.json()["detail"])

    def test_floor_round_trip_and_replace(self):
        self.setup_floor()
        got = self.client.get("/floor").json()
        self.assertEqual(got["nodes"], 8)
        self.assertEqual(len(got["edges"]), 10)
        self.setup_floor({"nodes": 3, "depot": 1, "capacity": 9, "edges": [{"u": 0, "v": 1, "length": 2}]})
        got = self.client.get("/floor").json()
        self.assertEqual((got["nodes"], got["depot"], got["capacity"], len(got["edges"])), (3, 1, 9, 1))

    def test_floor_validation(self):
        bad_edge = dict(FLOOR, edges=[{"u": 0, "v": 99, "length": 1}])
        self.assertEqual(self.client.put("/floor", json=bad_edge).status_code, 422)
        self.assertEqual(self.client.put("/floor", json=dict(FLOOR, depot=8)).status_code, 422)
        self.assertEqual(self.client.put("/floor", json=dict(FLOOR, capacity=-1)).status_code, 422)
        self.assertEqual(self.client.put("/floor", json=dict(FLOOR, nodes=0)).status_code, 422)
        self.assertEqual(self.client.put("/floor", json=dict(FLOOR, colour="red")).status_code, 422)  # unknown field
        negative = dict(FLOOR, edges=[{"u": 0, "v": 1, "length": -3}])
        self.assertEqual(self.client.put("/floor", json=negative).status_code, 422)

    def test_floor_cannot_shrink_below_a_product(self):
        self.setup_floor()
        self.add_product(1, node=7, unit_weight=1, unit_value=1)
        small = {"nodes": 4, "depot": 0, "capacity": 50, "edges": []}
        self.assertEqual(self.client.put("/floor", json=small).status_code, 409)
        self.assertEqual(self.client.get("/floor").json()["nodes"], 8)       # unchanged

    def test_path_and_network_come_from_the_engine(self):
        self.setup_floor()
        p = self.client.get("/floor/path", params={"source": 0, "target": 7}).json()
        self.assertTrue(p["reachable"])
        self.assertEqual(p["distance"], 13)
        self.assertEqual(p["path"][0], 0)
        self.assertEqual(self.client.get("/floor/path", params={"source": 0, "target": 99}).status_code, 422)
        k = self.client.get("/floor/network").json()
        pr = self.client.get("/floor/network", params={"algorithm": "prim"}).json()
        self.assertEqual((k["total_weight"], pr["total_weight"]), (23, 23))
        self.assertTrue(k["spanning"])
        self.assertEqual(self.client.get("/floor/network", params={"algorithm": "magic"}).status_code, 422)


class ProductsAndStock(ApiTestCase):
    def test_create_read_list_search(self):
        self.setup_floor()
        p = self.add_product(7, node=2, unit_weight=3, unit_value=9, quantity=12, name="Blue widget")
        self.assertEqual((p["quantity"], p["reserved"], p["available"]), (12, 0, 12))
        self.add_product(8, node=2, unit_weight=1, unit_value=1, name="Red 100%_gadget")
        self.assertEqual(self.client.get("/products/7").json()["name"], "Blue widget")
        self.assertEqual([x["sku"] for x in self.client.get("/products").json()], [7, 8])
        self.assertEqual([x["sku"] for x in self.client.get("/products", params={"search": "widget"}).json()], [7])
        # % and _ are searched literally, not as wildcards
        self.assertEqual([x["sku"] for x in self.client.get("/products", params={"search": "100%_"}).json()], [8])
        self.assertEqual(self.client.get("/products", params={"search": "%"}).json()[0]["sku"], 8)
        self.assertEqual(len(self.client.get("/products", params={"limit": 1, "offset": 1}).json()), 1)
        self.assertEqual(self.client.get("/products/999").status_code, 404)

    def test_create_validation(self):
        self.setup_floor()
        self.add_product(1, node=0, unit_weight=1, unit_value=1)
        base = {"sku": 2, "name": "x", "node": 0, "unit_weight": 1, "unit_value": 1}
        self.assertEqual(self.client.post("/products", json=dict(base, sku=1)).status_code, 409)       # duplicate
        self.assertEqual(self.client.post("/products", json=dict(base, node=50)).status_code, 422)     # off the floor
        self.assertEqual(self.client.post("/products", json=dict(base, name="")).status_code, 422)
        self.assertEqual(self.client.post("/products", json=dict(base, unit_weight=-1)).status_code, 422)
        self.assertEqual(self.client.post("/products", json=dict(base, quantity=-5)).status_code, 422)
        self.assertEqual(self.client.post("/products", json=dict(base, unit_weight=10**9)).status_code, 422)
        self.assertEqual(self.client.post("/products", json=dict(base, typo=1)).status_code, 422)
        del base["name"]
        self.assertEqual(self.client.post("/products", json=base).status_code, 422)

    def test_patch_product(self):
        self.setup_floor()
        self.add_product(1, node=1, unit_weight=2, unit_value=5)
        r = self.client.patch("/products/1", json={"name": "Renamed", "node": 4})
        self.assertEqual((r.json()["name"], r.json()["node"], r.json()["unit_weight"]), ("Renamed", 4, 2))
        self.assertEqual(self.client.patch("/products/1", json={"node": 99}).status_code, 422)
        self.assertEqual(self.client.patch("/products/1", json={"quantity": 500}).status_code, 422)  # stock is not editable here
        self.assertEqual(self.client.patch("/products/1", json={}).status_code, 200)                # nothing to change is fine
        self.assertEqual(self.client.patch("/products/77", json={"name": "x"}).status_code, 404)

    def test_restock_adjust_and_movement_history(self):
        self.setup_floor()
        self.add_product(1, node=1, unit_weight=1, unit_value=1, quantity=10)
        self.assertEqual(self.client.post("/products/1/restock", json={"quantity": 5}).json()["quantity"], 15)
        self.assertEqual(self.client.post("/products/1/adjust", json={"delta": -3, "reason": "damaged"}).json()["quantity"], 12)
        self.assertEqual(self.client.post("/products/1/adjust", json={"delta": -100, "reason": "oops"}).status_code, 409)
        self.assertEqual(self.client.post("/products/1/adjust", json={"delta": 0, "reason": "none"}).status_code, 422)
        self.assertEqual(self.client.post("/products/1/restock", json={"quantity": 0}).status_code, 422)
        self.assertEqual(self.client.post("/products/9/restock", json={"quantity": 1}).status_code, 404)
        moves = self.client.get("/movements", params={"sku": 1}).json()
        self.assertEqual([m["delta"] for m in moves], [-3, 5, 10])                 # newest first
        self.assertEqual(moves[0]["reason"], "adjustment: damaged")
        self.assertEqual(self.client.get("/movements", params={"sku": 2}).json(), [])

    def test_adjust_cannot_go_below_reserved_units(self):
        self.setup_floor()
        self.add_product(1, node=1, unit_weight=1, unit_value=1, quantity=10)
        self.add_order(1, quantity=6)
        r = self.client.post("/products/1/adjust", json={"delta": -5, "reason": "count"})   # would leave 5 < 6 reserved
        self.assertEqual(r.status_code, 409)
        self.assertIn("reserved", r.json()["detail"])
        self.assertEqual(self.client.post("/products/1/adjust", json={"delta": -4, "reason": "count"}).status_code, 200)

    def test_delete_product(self):
        self.setup_floor()
        self.add_product(1, node=1, unit_weight=1, unit_value=1)
        self.add_product(2, node=1, unit_weight=1, unit_value=1)
        self.add_order(1)
        self.assertEqual(self.client.delete("/products/1").status_code, 409)        # has an order
        self.assertEqual(self.client.delete("/products/2").status_code, 204)
        self.assertEqual(self.client.get("/products/2").status_code, 404)
        self.assertEqual(self.client.delete("/products/2").status_code, 404)
        self.assertTrue(len(self.client.get("/movements", params={"sku": 2}).json()) >= 1)   # history survives


class Orders(ApiTestCase):
    def test_order_lifecycle_fields(self):
        self.setup_floor()
        self.add_product(1, node=2, unit_weight=4, unit_value=7, quantity=10)
        o = self.add_order(1, quantity=3, urgency=2)
        self.assertEqual((o["status"], o["weight"], o["value"], o["trip_id"]), ("pending", 12, 21, None))
        self.assertEqual(self.client.get("/products/1").json()["reserved"], 3)
        self.assertEqual(self.client.get(f"/orders/{o['id']}").json()["quantity"], 3)
        self.assertEqual(self.client.get("/orders/99").status_code, 404)

    def test_stock_is_validated_against_available_not_total(self):
        self.setup_floor()
        self.add_product(1, node=2, unit_weight=1, unit_value=1, quantity=10)
        self.add_order(1, quantity=7)
        r = self.client.post("/orders", json={"sku": 1, "quantity": 4})                  # only 3 left unreserved
        self.assertEqual(r.status_code, 409)
        self.assertIn("only 3", r.json()["detail"])
        self.assertEqual(self.client.post("/orders", json={"sku": 1, "quantity": 3}).status_code, 201)

    def test_order_validation(self):
        self.setup_floor()
        self.add_product(1, node=2, unit_weight=20, unit_value=1, quantity=100)
        self.assertEqual(self.client.post("/orders", json={"sku": 99, "quantity": 1}).status_code, 404)
        self.assertEqual(self.client.post("/orders", json={"sku": 1, "quantity": 0}).status_code, 422)
        self.assertEqual(self.client.post("/orders", json={"sku": 1, "quantity": 20000}).status_code, 422)
        self.assertEqual(self.client.post("/orders", json={"sku": 1, "quantity": 1, "urgency": 99999}).status_code, 422)
        heavy = self.client.post("/orders", json={"sku": 1, "quantity": 3})               # 60 > robot capacity 50
        self.assertEqual(heavy.status_code, 409)
        self.assertIn("split", heavy.json()["detail"])

    def test_cancel_and_filters(self):
        self.setup_floor()
        self.add_product(1, node=2, unit_weight=1, unit_value=1, quantity=10)
        a, b = self.add_order(1), self.add_order(1)
        self.assertEqual(self.client.post(f"/orders/{a['id']}/cancel").json()["status"], "cancelled")
        self.assertEqual(self.client.post(f"/orders/{a['id']}/cancel").status_code, 409)      # already cancelled
        self.assertEqual(self.client.get("/products/1").json()["reserved"], 1)                # cancelled stock is released
        self.assertEqual([o["id"] for o in self.client.get("/orders", params={"status": "pending"}).json()], [b["id"]])
        self.assertEqual(len(self.client.get("/orders", params={"sku": 1}).json()), 2)
        self.assertEqual(self.client.get("/orders", params={"status": "bogus"}).status_code, 422)
        self.assertEqual(self.client.post("/orders/99/cancel").status_code, 404)


class Trips(ApiTestCase):
    def test_plan_trip_picks_the_optimal_mix(self):
        self.classic_setup()
        r = self.client.post("/trips/plan")
        self.assertEqual(r.status_code, 200, r.text)
        plan = r.json()
        trip = plan["trip"]
        self.assertEqual(trip["order_ids"], [2, 3])              # the 20kg + 30kg pair: value 220, not the greedy 160
        self.assertEqual((trip["total_value"], trip["total_weight"], trip["distance"]), (220, 50, 26))
        self.assertEqual(trip["status"], "planned")
        self.assertEqual((trip["route"][0], trip["route"][-1]), (0, 0))
        self.assertEqual({s["node"] for s in trip["stops"]}, {6, 7})
        self.assertEqual(plan["deferred"], [1])
        self.assertEqual(plan["rejected"], [])
        self.assertIn("elapsed_ms", trip["stats"])
        statuses = {o["id"]: o["status"] for o in self.client.get("/orders").json()}
        self.assertEqual(statuses, {1: "pending", 2: "planned", 3: "planned"})
        self.assertEqual(self.client.get("/orders/2").json()["trip_id"], trip["id"])
        self.assertEqual(self.client.get(f"/trips/{trip['id']}").json()["distance"], 26)

    def test_second_plan_takes_what_is_left(self):
        self.classic_setup()
        self.client.post("/trips/plan")
        second = self.client.post("/trips/plan").json()
        self.assertEqual(second["trip"]["order_ids"], [1])
        self.assertEqual(second["trip"]["distance"], 2 * 12)       # shelf at node 3 is 12 away from the depot
        self.assertEqual(self.client.post("/trips/plan").status_code, 409)      # nothing pending any more
        self.assertEqual(len(self.client.get("/trips").json()), 2)
        self.assertEqual(len(self.client.get("/trips", params={"status": "planned"}).json()), 2)

    def test_complete_trip_updates_stock_orders_and_history(self):
        self.classic_setup()
        trip_id = self.client.post("/trips/plan").json()["trip"]["id"]
        done = self.client.post(f"/trips/{trip_id}/complete")
        self.assertEqual(done.status_code, 200, done.text)
        self.assertEqual(done.json()["status"], "completed")
        self.assertIsNotNone(done.json()["finished_at"])
        self.assertEqual(self.client.get("/products/102").json()["quantity"], 4)       # 5 - 1
        self.assertEqual(self.client.get("/products/103").json()["quantity"], 4)
        self.assertEqual(self.client.get("/products/101").json()["quantity"], 5)       # not on this trip
        self.assertEqual(self.client.get("/orders/2").json()["status"], "completed")
        picks = [m for m in self.client.get("/movements", params={"sku": 102}).json() if m["reason"] == "pick"]
        self.assertEqual((len(picks), picks[0]["delta"], picks[0]["order_id"]), (1, -1, 2))
        self.assertEqual(self.client.post(f"/trips/{trip_id}/complete").status_code, 409)   # not twice
        self.assertEqual(self.client.post(f"/trips/{trip_id}/cancel").status_code, 409)

    def test_cancel_trip_returns_orders_to_pending(self):
        self.classic_setup()
        trip_id = self.client.post("/trips/plan").json()["trip"]["id"]
        self.assertEqual(self.client.post("/orders/2/cancel").status_code, 409)             # planned: cancel the trip instead
        c = self.client.post(f"/trips/{trip_id}/cancel")
        self.assertEqual(c.json()["status"], "cancelled")
        self.assertEqual({o["status"] for o in self.client.get("/orders").json()}, {"pending"})
        self.assertIsNone(self.client.get("/orders/2").json()["trip_id"])
        self.assertEqual(self.client.post(f"/trips/{trip_id}/cancel").status_code, 409)
        again = self.client.post("/trips/plan").json()                                       # and they can be planned again
        self.assertEqual(again["trip"]["order_ids"], [2, 3])
        self.assertEqual(self.client.get("/products/102").json()["quantity"], 5)             # stock never moved

    def test_complete_is_all_or_nothing(self):
        self.classic_setup()
        trip_id = self.client.post("/trips/plan").json()["trip"]["id"]
        self.sql("UPDATE products SET quantity = 0 WHERE sku = 103")       # stock vanishes behind the API's back
        r = self.client.post(f"/trips/{trip_id}/complete")
        self.assertEqual(r.status_code, 409)
        self.assertIn("not enough stock", r.json()["detail"])
        # order 2 is processed before order 3, so it must have been rolled back
        self.assertEqual(self.client.get("/products/102").json()["quantity"], 5)
        self.assertEqual(self.client.get("/orders/2").json()["status"], "planned")
        self.assertEqual(self.client.get(f"/trips/{trip_id}").json()["status"], "planned")
        self.assertEqual([m for m in self.client.get("/movements", params={"sku": 102}).json() if m["reason"] == "pick"], [])

    def test_rejected_orders_when_the_shelf_cannot_be_reached(self):
        floor = {"nodes": 5, "depot": 0, "capacity": 50, "edges": [{"u": 0, "v": 1, "length": 3}]}   # nodes 2,3,4 are cut off
        self.setup_floor(floor)
        self.add_product(1, node=1, unit_weight=5, unit_value=10)
        self.add_product(2, node=4, unit_weight=5, unit_value=10)
        ok_order = self.add_order(1)
        lost_order = self.add_order(2)
        plan = self.client.post("/trips/plan").json()
        self.assertEqual(plan["trip"]["order_ids"], [ok_order["id"]])
        self.assertEqual(plan["rejected"], [{"order_id": lost_order["id"], "reason": "unreachable_from_depot"}])
        rejected = self.client.get(f"/orders/{lost_order['id']}").json()
        self.assertEqual((rejected["status"], rejected["note"]), ("rejected", "unreachable_from_depot"))
        self.assertEqual(self.client.get("/products/2").json()["reserved"], 0)                # rejected orders release their stock

    def test_all_orders_rejected_gives_no_trip(self):
        self.setup_floor({"nodes": 3, "depot": 0, "capacity": 50, "edges": [{"u": 0, "v": 1, "length": 1}]})
        self.add_product(1, node=2, unit_weight=1, unit_value=1)                               # node 2 is isolated
        self.add_order(1)
        plan = self.client.post("/trips/plan").json()
        self.assertIsNone(plan["trip"])
        self.assertEqual(len(plan["rejected"]), 1)
        self.assertEqual(self.client.get("/trips").json(), [])

    def test_changing_things_under_a_planned_trip_is_refused(self):
        self.classic_setup()
        self.client.post("/trips/plan")
        self.assertEqual(self.client.put("/floor", json=FLOOR).status_code, 409)
        self.assertEqual(self.client.patch("/products/102", json={"node": 1}).status_code, 409)
        self.assertEqual(self.client.patch("/products/102", json={"name": "New name"}).status_code, 200)   # harmless change is fine
        self.assertEqual(self.client.patch("/products/101", json={"node": 1}).status_code, 200)            # 101 is not on the trip

    def test_max_candidates_limits_the_engine_pool(self):
        self.classic_setup()
        self.assertEqual(self.client.post("/trips/plan", params={"max_candidates": 0}).status_code, 422)
        self.assertEqual(self.client.post("/trips/plan", params={"max_candidates": 15}).status_code, 422)
        # Only the single most urgent order (id 3) is offered to the engine.
        plan = self.client.post("/trips/plan", params={"max_candidates": 1}).json()
        self.assertEqual(plan["trip"]["order_ids"], [3])
        self.assertEqual(sorted(plan["deferred"]), [1, 2])

    def test_missing_trip(self):
        self.assertEqual(self.client.get("/trips/42").status_code, 404)
        self.assertEqual(self.client.post("/trips/42/complete").status_code, 404)
        self.assertEqual(self.client.post("/trips/42/cancel").status_code, 404)

    def test_stats(self):
        self.classic_setup()
        self.client.post("/trips/plan")
        s = self.client.get("/stats").json()
        self.assertEqual(s["products"], 3)
        self.assertEqual(s["units_in_stock"], 15)
        self.assertEqual((s["orders"]["pending"], s["orders"]["planned"]), (1, 2))
        self.assertEqual(s["trips"]["planned"], 1)


class EngineFailure(unittest.TestCase):
    def test_a_missing_engine_gives_502_and_leaves_data_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "t.db")
            with TestClient(create_app(db_path=db, engine_path=os.path.join(tmp, "no_such_engine"))) as client:
                self.assertEqual(client.put("/floor", json=FLOOR).status_code, 200)
                client.post("/products", json={"sku": 1, "name": "a", "node": 3, "quantity": 5,
                                               "unit_weight": 1, "unit_value": 1})
                client.post("/orders", json={"sku": 1, "quantity": 1})
                h = client.get("/health")
                self.assertEqual(h.status_code, 503)
                self.assertEqual(h.json()["engine"], "unavailable")
                r = client.post("/trips/plan")
                self.assertEqual(r.status_code, 502)
                self.assertIn("engine", r.json()["detail"])
                self.assertEqual(client.get("/orders/1").json()["status"], "pending")        # rolled back
                self.assertEqual(client.get("/trips").json(), [])
                self.assertEqual(client.get("/products").status_code, 200)                   # the rest of the API still works


class Cors(unittest.TestCase):
    def test_the_published_site_may_call_the_api_and_strangers_may_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            with TestClient(create_app(db_path=os.path.join(tmp, "t.db"), engine_path=ENGINE)) as client:
                ok = client.get("/stats", headers={"Origin": "https://ananyaparihar.github.io"})
                self.assertEqual(ok.headers.get("access-control-allow-origin"), "https://ananyaparihar.github.io")
                other = client.get("/stats", headers={"Origin": "https://evil.example"})
                self.assertIsNone(other.headers.get("access-control-allow-origin"))


if __name__ == "__main__":
    unittest.main()
