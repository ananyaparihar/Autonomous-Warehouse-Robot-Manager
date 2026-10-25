"""Demo: send a small warehouse scenario through the C++ engine from Python.

Run (from the api folder):   py demo_integration.py
Needs the engine built first (see engine/README.md).
"""

from engine_client import EngineError, network_mst, plan_trip, shortest_path

# The 8-node warehouse floor used in the C++ tests.
# Each edge is [node_a, node_b, length]. Node 0 is the depot where the robot starts.
FLOOR = {
    "nodes": 8,
    "edges": [
        [0, 1, 4], [1, 2, 3], [2, 3, 5], [0, 4, 2], [4, 5, 6],
        [5, 6, 2], [6, 7, 4], [1, 5, 3], [3, 7, 6], [2, 6, 4],
    ],
}

# Which floor node each SKU's bin is at (this is what the B+ Tree index holds).
INVENTORY = [
    {"sku": 101, "node": 3},
    {"sku": 102, "node": 6},
    {"sku": 103, "node": 7},
]

# Pending orders. Lower urgency number = more urgent.
ORDERS = [
    {"id": 1, "sku": 101, "weight": 10, "value": 60, "urgency": 3},
    {"id": 2, "sku": 102, "weight": 20, "value": 100, "urgency": 2},
    {"id": 3, "sku": 103, "weight": 30, "value": 120, "urgency": 1},
    {"id": 4, "sku": 999, "weight": 5, "value": 10, "urgency": 1},  # SKU not in inventory
]


def main() -> None:
    print("=== plan_trip: robot payload limit 50 ===")
    plan = plan_trip(FLOOR, capacity=50, inventory=INVENTORY, orders=ORDERS)
    print(f"orders on this trip : {plan['selected']}  (value {plan['total_value']}, weight {plan['total_weight']})")
    print(f"left for later      : {plan['deferred']}")
    print(f"rejected            : {plan['rejected']}")
    print(f"stops in order      : {[(s['node'], s['orders']) for s in plan['stops']]}")
    print(f"floor route         : {' -> '.join(map(str, plan['route']))}")
    print(f"total distance      : {plan['distance']}")
    print(f"B&B search          : {plan['stats']['bb_nodes_explored']} nodes explored, "
          f"{plan['stats']['bb_nodes_pruned']} pruned")
    print(f"engine time         : {plan['elapsed_ms']} ms")

    print("\n=== shortest_path: depot (0) to shelf 7 ===")
    path = shortest_path(FLOOR, 0, 7)
    print(f"distance {path['distance']} via {path['path']}")

    print("\n=== network_mst: cheapest conveyor network linking every node ===")
    mst = network_mst(FLOOR)
    print(f"total length {mst['total_weight']} using {len(mst['edges'])} links: {mst['edges']}")

    print("\n=== error handling: a request the engine must reject ===")
    try:
        plan_trip(FLOOR, capacity=-5, inventory=INVENTORY, orders=ORDERS)
    except EngineError as exc:
        print(f"EngineError: {exc}")


if __name__ == "__main__":
    try:
        main()
    except EngineError as exc:
        raise SystemExit(f"Engine problem: {exc}")
