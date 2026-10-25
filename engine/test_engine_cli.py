"""End-to-end tests: Python -> engine_cli -> Python.

Run (from the api folder):   py -m unittest test_engine_cli -v
These tests are skipped if the engine program has not been built yet.
"""

import json
import os
import subprocess
import unittest

from engine_client import (
    EngineError,
    call_engine,
    default_engine_path,
    lookup_sku,
    network_mst,
    ping,
    plan_trip,
    shortest_path,
    sku_range,
)

ENGINE = os.environ.get("WAREHOUSE_ENGINE") or str(default_engine_path())
HAVE_ENGINE = os.path.isfile(ENGINE)

FLOOR = {
    "nodes": 8,
    "edges": [
        [0, 1, 4], [1, 2, 3], [2, 3, 5], [0, 4, 2], [4, 5, 6],
        [5, 6, 2], [6, 7, 4], [1, 5, 3], [3, 7, 6], [2, 6, 4],
    ],
}
INVENTORY = [{"sku": 101, "node": 3}, {"sku": 102, "node": 6}, {"sku": 103, "node": 7}]
ORDERS = [
    {"id": 1, "sku": 101, "weight": 10, "value": 60, "urgency": 3},
    {"id": 2, "sku": 102, "weight": 20, "value": 100, "urgency": 2},
    {"id": 3, "sku": 103, "weight": 30, "value": 120, "urgency": 1},
]


def raw_run(text: str, *args: str) -> "subprocess.CompletedProcess[bytes]":
    return subprocess.run([ENGINE, *args], input=text.encode("utf-8"),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)


@unittest.skipUnless(HAVE_ENGINE, "engine program not built (see engine/README.md)")
class EngineEndToEnd(unittest.TestCase):
    def test_ping(self):
        r = ping()
        self.assertTrue(r["ok"])
        self.assertEqual(r["engine"], "warehouse-engine")

    def test_plan_trip_classic_example(self):
        r = plan_trip(FLOOR, capacity=50, inventory=INVENTORY, orders=ORDERS)
        self.assertEqual(r["selected"], [3, 2])          # most urgent first
        self.assertEqual(r["deferred"], [1])
        self.assertEqual(r["total_value"], 220)          # the 20kg + 30kg pair beats greedy's 160
        self.assertEqual(r["total_weight"], 50)
        self.assertEqual(r["distance"], 26)
        self.assertEqual(r["route"][0], 0)
        self.assertEqual(r["route"][-1], 0)
        self.assertEqual({s["node"] for s in r["stops"]}, {6, 7})
        self.assertIn("elapsed_ms", r)

    def test_route_is_a_real_walk_on_the_floor(self):
        r = plan_trip(FLOOR, capacity=1000, inventory=INVENTORY, orders=ORDERS)
        length = {}
        for u, v, w in FLOOR["edges"]:
            for key in ((u, v), (v, u)):
                length[key] = min(w, length.get(key, w))
        route = r["route"]
        total = sum(length[(route[i], route[i + 1])] for i in range(len(route) - 1))
        self.assertEqual(total, r["distance"])
        for stop in r["stops"]:
            self.assertIn(stop["node"], route)

    def test_rejected_orders_are_reported(self):
        orders = ORDERS + [{"id": 9, "sku": 555, "weight": 1, "value": 1, "urgency": 0}]
        r = plan_trip(FLOOR, capacity=50, inventory=INVENTORY, orders=orders)
        self.assertEqual(r["rejected"], [{"id": 9, "reason": "unknown_sku"}])

    def test_shortest_path(self):
        r = shortest_path(FLOOR, 0, 7)
        self.assertTrue(r["reachable"])
        self.assertEqual(r["distance"], 13)
        self.assertEqual(r["path"][0], 0)
        self.assertEqual(r["path"][-1], 7)

    def test_shortest_path_unreachable(self):
        r = shortest_path({"nodes": 3, "edges": [[0, 1, 5]]}, 0, 2)
        self.assertFalse(r["reachable"])
        self.assertIsNone(r["distance"])
        self.assertEqual(r["path"], [])

    def test_network_mst_both_algorithms_agree(self):
        k = network_mst(FLOOR, "kruskal")
        p = network_mst(FLOOR, "prim")
        self.assertEqual(k["total_weight"], 23)
        self.assertEqual(p["total_weight"], 23)
        self.assertTrue(k["spanning"])
        self.assertEqual(len(k["edges"]), 7)

    def test_lookup_and_range(self):
        inv = [{"sku": s, "node": s % 8} for s in range(1000, 1100)]
        hit = lookup_sku(inv, 1042)
        self.assertTrue(hit["found"])
        self.assertEqual(hit["node"], 1042 % 8)
        miss = lookup_sku(inv, 5)
        self.assertFalse(miss["found"])
        self.assertIsNone(miss["node"])
        rng = sku_range(inv, 1010, 1019)
        self.assertEqual(rng["count"], 10)
        self.assertEqual([i["sku"] for i in rng["items"]], list(range(1010, 1020)))
        capped = sku_range(inv, 1000, 1099, limit=5)
        self.assertEqual(capped["count"], 100)
        self.assertTrue(capped["truncated"])
        self.assertEqual(len(capped["items"]), 5)

    def test_bad_requests_raise_engine_error(self):
        with self.assertRaises(EngineError):
            call_engine({"op": "fly"})
        with self.assertRaises(EngineError):
            call_engine({"op": "shortest_path", "floor": FLOOR, "from": 0})   # missing "to"
        with self.assertRaises(EngineError):
            plan_trip(FLOOR, capacity=-1, inventory=INVENTORY, orders=ORDERS)
        with self.assertRaises(EngineError):
            plan_trip(FLOOR, capacity=50, inventory=INVENTORY, orders=ORDERS, depot=99)
        with self.assertRaises(EngineError):
            plan_trip(FLOOR, capacity=50, inventory=INVENTORY, orders=ORDERS, max_candidates=100)
        with self.assertRaises(EngineError):
            call_engine({"op": "plan_trip", "floor": {"nodes": 2, "edges": [[0, 5, 1]]},
                         "capacity": 1, "inventory": [], "orders": []})
        with self.assertRaises(EngineError):
            call_engine({"op": "plan_trip", "floor": FLOOR, "capacity": "lots", "inventory": [], "orders": []})

    def test_malformed_json_and_exit_codes(self):
        bad = raw_run("{ not json")
        self.assertEqual(bad.returncode, 1)
        body = json.loads(bad.stdout)
        self.assertFalse(body["ok"])
        self.assertIn("invalid JSON", body["error"])

        good = raw_run('{"op":"ping"}')
        self.assertEqual(good.returncode, 0)

        usage = raw_run("", "--nonsense")
        self.assertEqual(usage.returncode, 2)

    def test_pretty_flag_and_file_mode(self):
        pretty = raw_run('{"op":"ping"}', "--pretty")
        self.assertIn(b"\n  ", pretty.stdout)
        self.assertTrue(json.loads(pretty.stdout)["ok"])
        missing = raw_run("", "--file", "does_not_exist.json")
        self.assertEqual(missing.returncode, 1)
        self.assertFalse(json.loads(missing.stdout)["ok"])

    def test_large_request_does_not_deadlock(self):
        # About 100,000 inventory entries (several MB of JSON) through the pipe.
        nodes = 500
        floor = {"nodes": nodes, "edges": [[i, i + 1, 1 + i % 7] for i in range(nodes - 1)]}
        inventory = [{"sku": s, "node": s % nodes} for s in range(100_000)]
        orders = [{"id": i, "sku": (i * 7919) % 100_000, "weight": 5 + i % 20,
                   "value": 1 + i % 50, "urgency": i % 5} for i in range(200)]
        r = plan_trip(floor, capacity=100, inventory=inventory, orders=orders, timeout=60)
        self.assertLessEqual(r["total_weight"], 100)
        self.assertEqual(len(r["selected"]) + len(r["deferred"]) + len(r["rejected"]), 200)

    def test_missing_engine_gives_a_clear_error(self):
        with self.assertRaises(EngineError) as ctx:
            call_engine({"op": "ping"}, engine_path="no/such/engine_cli")
        self.assertIn("not found", str(ctx.exception))

    def test_unicode_in_unused_fields_is_accepted(self):
        r = call_engine({"op": "ping", "note": "caf\u00e9 \u2615 \U0001F600"})
        self.assertTrue(r["ok"])


if __name__ == "__main__":
    unittest.main()
