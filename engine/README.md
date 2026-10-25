# Warehouse Robot Manager: DSA engine

A C++ engine that plans one warehouse-robot trip by chaining the data
structures and algorithms from the course, plus a small Python layer so a web
backend (FastAPI) can call it.

```
warehouse/
  engine/     C++ engine, one header per module, each with its own test file
  api/        Python wrapper (engine_client.py), demo and end-to-end tests
  examples/   sample JSON request
```

## How one trip is planned

```
orders -> [B+ Tree]        SKU -> shelf location (floor node)
       -> [Union-Find]     drop orders whose shelf the robot can't reach
       -> [Min-Heap]       most urgent orders become the candidate pool
       -> [Knapsack DP]    best-value mix that fits the robot's payload
       -> [Dijkstra]       real floor distance between every pair of stops
       -> [Branch & Bound] best order to visit the stops (depot -> ... -> depot)
       -> [Dijkstra]       expand into the full list of floor nodes to drive
```

| Module | File | Rubric topic |
|---|---|---|
| Min-heap | `min_heap.hpp` | I: heaps (urgency queue) |
| AVL tree + traversals | `avl_tree.hpp` | I: tree structures, traversals |
| Dijkstra | `dijkstra.hpp` | II: shortest path |
| Kruskal / Prim | `mst.hpp` | II: MST |
| 0/1 Knapsack | `knapsack.hpp` | III: dynamic programming |
| Branch and Bound | `branch_bound.hpp` | IV: multi-stop route |
| B+ Tree | `bplus_tree.hpp` | V: advanced structures (inventory index) |
| Pipeline | `trip_planner.hpp` | integration |
| JSON + command line | `json.hpp`, `engine_cli.cpp` | integration |

## Build and test

Needs `g++` (C++14). From the `engine` folder:

```
py run_all_tests.py          # Windows: compiles and runs every test, builds engine_cli.exe
python3 run_all_tests.py     # Linux / macOS
```

Or build just the program:

```
g++ -std=c++14 -O2 engine_cli.cpp -o engine_cli
```

From the `api` folder (after building the engine):

```
py demo_integration.py
py -m unittest test_engine_cli -v
```

## Using the engine

`engine_cli` reads one JSON request and prints one JSON response.

```
engine_cli                       request on stdin
engine_cli --file request.json   request from a file (easiest in PowerShell)
engine_cli --pretty              indent the output
```

Try it: `.\engine_cli.exe --pretty --file ..\examples\plan_trip_example.json`

Every response has `"ok"`. On failure: `{"ok": false, "error": "..."}` and exit code 1.
Successful responses also carry `elapsed_ms`.

### Floor format (used by several operations)

```json
"floor": { "nodes": 8, "edges": [[0,1,4], [1,2,3]] }
```
Nodes are numbered `0..nodes-1`. Each edge is `[node_a, node_b, length]`
(undirected, length >= 0).

### `plan_trip`

Request:

| Field | Meaning |
|---|---|
| `floor` | the warehouse floor graph |
| `depot` | node where the robot starts and ends (default 0) |
| `capacity` | robot payload limit, 0 to 1,000,000 |
| `max_candidates` | orders the heap offers the knapsack, 1 to 14 (default 12) |
| `inventory` | `[{"sku": 101, "node": 3}, ...]`: which node each SKU is at |
| `orders` | `[{"id": 1, "sku": 101, "weight": 10, "value": 60, "urgency": 3}, ...]` (lower urgency number = more urgent; `id` and `urgency` optional) |

Response:

| Field | Meaning |
|---|---|
| `selected` | order ids on this trip, most urgent first |
| `deferred` | valid orders left for a later trip |
| `rejected` | `[{"id", "reason"}]`, reason is `unknown_sku` or `unreachable_from_depot` |
| `stops` | `[{"node", "orders"}]` in visiting order |
| `route` | the full path of floor nodes, depot to depot |
| `distance` | length of `route` |
| `total_value`, `total_weight` | of the selected orders |
| `stats` | `candidates`, `bb_nodes_explored`, `bb_nodes_pruned` |

### Other operations

| `op` | Extra request fields | Response fields |
|---|---|---|
| `ping` | none | `engine`, `version` |
| `shortest_path` | `floor`, `from`, `to` | `reachable`, `distance` (null if unreachable), `path` |
| `network_mst` | `floor`, `algorithm` (`kruskal` or `prim`) | `spanning`, `components`, `total_weight`, `edges` |
| `lookup_sku` | `inventory`, `sku` | `found`, `node` |
| `sku_range` | `inventory`, `lo`, `hi`, `limit` (default 1000) | `count`, `truncated`, `items` |

### From Python

```python
from engine_client import plan_trip
plan = plan_trip(floor, capacity=50, inventory=inventory, orders=orders)
print(plan["route"], plan["distance"])
```
Errors raise `EngineError`. Set the `WAREHOUSE_ENGINE` environment variable to
point at the program if it is not in `engine/`.

## Design notes and limits

* **Stateless.** Each call starts the program, so the caller keeps the data
  (SQLite later) and sends what the engine needs. The inventory is re-sent and
  the B+ Tree index rebuilt on every call. Measured per `plan_trip` call:
  10,000 SKUs about 0.04 s, 100,000 SKUs about 0.25 s, 1,000,000 SKUs about 2 s
  (roughly half of that is sending and parsing the JSON). If that becomes too
  slow, the upgrade is a long-running engine process that keeps the index in memory.
* **Branch and Bound is factorial in the worst case**, so a trip is limited to
  14 candidate orders (at most 14 stops). It handled 16 stops in about 0.4 s in the benchmark.
* **The knapsack table** is candidates x capacity, hence the capacity limit.
* **Unreachable shelves** (floor pieces not connected to the depot) are
  rejected, not planned.
* **Floor is undirected**; one-way aisles would need a directed graph.
