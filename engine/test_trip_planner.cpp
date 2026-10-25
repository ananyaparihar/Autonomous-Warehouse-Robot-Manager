// test_trip_planner.cpp - end-to-end tests for the whole planning pipeline.
// Needs all the engine headers in the same folder.
// Build:  g++ -std=c++14 -O2 test_trip_planner.cpp -o test_trip_planner
// Run:    .\test_trip_planner.exe

#include "trip_planner.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <set>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

static FloorEdge E(int u, int v, int w) { FloorEdge e; e.u = u; e.v = v; e.w = w; return e; }
static SkuLocation S(int sku, int node) { SkuLocation s; s.sku = sku; s.node = node; return s; }
static PlanOrder O(long long id, int sku, int weight, int value, int urgency) {
    PlanOrder o; o.id = id; o.sku = sku; o.weight = weight; o.value = value; o.urgency = urgency; return o;
}

// The 8-node warehouse floor used in the earlier module tests.
static TripRequest warehouseRequest() {
    TripRequest r;
    r.nodes = 8;
    r.edges.push_back(E(0, 1, 4)); r.edges.push_back(E(1, 2, 3)); r.edges.push_back(E(2, 3, 5));
    r.edges.push_back(E(0, 4, 2)); r.edges.push_back(E(4, 5, 6)); r.edges.push_back(E(5, 6, 2));
    r.edges.push_back(E(6, 7, 4)); r.edges.push_back(E(1, 5, 3)); r.edges.push_back(E(3, 7, 6));
    r.edges.push_back(E(2, 6, 4));
    r.depot = 0;
    r.inventory.push_back(S(101, 3)); r.inventory.push_back(S(102, 6)); r.inventory.push_back(S(103, 7));
    return r;
}

// Is plan.route a real walk on the floor that starts and ends at the depot,
// whose length equals plan.distance, and which passes through every stop?
static bool routeIsValid(const TripRequest& req, const TripPlan& plan) {
    if (plan.route.empty() || plan.route.front() != req.depot || plan.route.back() != req.depot) return false;
    std::map<std::pair<int, int>, int> shortest;                  // cheapest edge between two nodes
    for (std::size_t i = 0; i < req.edges.size(); ++i) {
        std::pair<int, int> a(req.edges[i].u, req.edges[i].v), b(req.edges[i].v, req.edges[i].u);
        for (int k = 0; k < 2; ++k) {
            std::pair<int, int> key = k == 0 ? a : b;
            std::map<std::pair<int, int>, int>::iterator it = shortest.find(key);
            if (it == shortest.end() || req.edges[i].w < it->second) shortest[key] = req.edges[i].w;
        }
    }
    long long total = 0;
    for (std::size_t i = 0; i + 1 < plan.route.size(); ++i) {
        std::map<std::pair<int, int>, int>::iterator it =
            shortest.find(std::make_pair(plan.route[i], plan.route[i + 1]));
        if (it == shortest.end()) return false;                    // consecutive nodes are not joined by an edge
        total += it->second;
    }
    if (total != plan.distance) return false;
    std::set<int> visited(plan.route.begin(), plan.route.end());
    for (std::size_t s = 0; s < plan.stops.size(); ++s)
        if (visited.find(plan.stops[s].node) == visited.end()) return false;
    return true;
}

void test_classic_knapsack_through_the_pipeline() {
    // Three orders, payload 50: the best mix is the 20kg + 30kg pair (value 220), not the greedy 10kg + 20kg.
    TripRequest r = warehouseRequest();
    r.capacity = 50;
    r.orders.push_back(O(1, 101, 10, 60, 3));
    r.orders.push_back(O(2, 102, 20, 100, 2));
    r.orders.push_back(O(3, 103, 30, 120, 1));
    TripPlan p = planTrip(r);

    CHECK(p.totalValue == 220);
    CHECK(p.totalWeight == 50);
    CHECK(p.selected.size() == 2);
    CHECK(p.selected[0] == 3 && p.selected[1] == 2);               // most urgent first
    CHECK(p.deferred.size() == 1 && p.deferred[0] == 1);
    CHECK(p.rejected.empty());
    CHECK(p.stops.size() == 2);
    // Shelves at nodes 6 and 7. Floor distances: depot->6 = 9, 6->7 = 4, 7->depot = 13. Total 26.
    CHECK(p.distance == 26);
    CHECK(routeIsValid(r, p));
    CHECK(p.candidates == 3);
}

void test_everything_fits() {
    TripRequest r = warehouseRequest();
    r.capacity = 1000;
    r.orders.push_back(O(1, 101, 10, 60, 3));
    r.orders.push_back(O(2, 102, 20, 100, 2));
    r.orders.push_back(O(3, 103, 30, 120, 1));
    TripPlan p = planTrip(r);
    CHECK(p.selected.size() == 3 && p.deferred.empty());
    CHECK(p.totalValue == 280);
    CHECK(p.stops.size() == 3);
    CHECK(routeIsValid(r, p));
}

void test_zero_capacity_and_no_orders() {
    TripRequest r = warehouseRequest();
    r.capacity = 0;
    r.orders.push_back(O(1, 101, 10, 60, 3));
    TripPlan p = planTrip(r);
    CHECK(p.selected.empty());
    CHECK(p.deferred.size() == 1);
    CHECK(p.distance == 0);
    CHECK(p.route.size() == 1 && p.route[0] == 0);

    TripRequest none = warehouseRequest();
    none.capacity = 50;
    TripPlan q = planTrip(none);
    CHECK(q.selected.empty() && q.deferred.empty() && q.rejected.empty());
    CHECK(q.route.size() == 1 && q.distance == 0);
}

void test_rejected_orders() {
    TripRequest r = warehouseRequest();
    r.nodes = 10;                                       // nodes 8 and 9 exist but are not connected to anything
    r.inventory.push_back(S(104, 9));                   // SKU 104 sits on the isolated node
    r.capacity = 100;
    r.orders.push_back(O(1, 101, 10, 60, 1));
    r.orders.push_back(O(2, 999, 10, 60, 1));           // unknown SKU
    r.orders.push_back(O(3, 104, 10, 60, 1));           // unreachable shelf
    TripPlan p = planTrip(r);
    CHECK(p.selected.size() == 1 && p.selected[0] == 1);
    CHECK(p.rejected.size() == 2);
    CHECK(p.rejected[0].id == 2 && p.rejected[0].reason == "unknown_sku");
    CHECK(p.rejected[1].id == 3 && p.rejected[1].reason == "unreachable_from_depot");
    CHECK(routeIsValid(r, p));
}

void test_heap_limits_the_candidate_pool() {
    // Four orders; the LEAST urgent one is by far the most valuable. With a pool of 2
    // the heap only offers the two most urgent, so the valuable one must wait.
    TripRequest r = warehouseRequest();
    r.inventory.push_back(S(104, 1));
    r.capacity = 100;
    r.maxCandidates = 2;
    r.orders.push_back(O(1, 101, 5, 10, 5));
    r.orders.push_back(O(2, 102, 5, 10, 1));
    r.orders.push_back(O(3, 103, 5, 10, 2));
    r.orders.push_back(O(4, 104, 5, 1000, 9));
    TripPlan p = planTrip(r);
    CHECK(p.candidates == 2);
    CHECK(p.selected.size() == 2 && p.selected[0] == 2 && p.selected[1] == 3);
    CHECK(p.totalValue == 20);
    CHECK(p.deferred.size() == 2 && p.deferred[0] == 1 && p.deferred[1] == 4);   // heap order: urgency 5 then 9

    r.maxCandidates = 4;                                 // now the valuable order is in the pool and gets picked
    TripPlan q = planTrip(r);
    CHECK(q.totalValue == 1030);
}

void test_equal_urgency_is_first_come_first_served() {
    TripRequest r = warehouseRequest();
    r.capacity = 100;
    r.maxCandidates = 2;
    r.orders.push_back(O(10, 101, 1, 1, 4));
    r.orders.push_back(O(11, 102, 1, 1, 4));
    r.orders.push_back(O(12, 103, 1, 1, 4));
    TripPlan p = planTrip(r);
    CHECK(p.selected.size() == 2 && p.selected[0] == 10 && p.selected[1] == 11);
    CHECK(p.deferred.size() == 1 && p.deferred[0] == 12);
}

void test_orders_sharing_a_shelf_make_one_stop() {
    TripRequest r = warehouseRequest();
    r.inventory.push_back(S(105, 6));                    // second SKU on the same shelf node as 102
    r.capacity = 100;
    r.orders.push_back(O(1, 102, 5, 10, 1));
    r.orders.push_back(O(2, 105, 5, 10, 2));
    TripPlan p = planTrip(r);
    CHECK(p.stops.size() == 1);
    CHECK(p.stops[0].node == 6 && p.stops[0].orderIds.size() == 2);
    CHECK(p.distance == 18);                             // depot -> 6 -> depot = 9 + 9
    CHECK(routeIsValid(r, p));
}

void test_orders_at_the_depot_cost_nothing() {
    TripRequest r = warehouseRequest();
    r.inventory.push_back(S(106, 0));                    // shelf is the depot itself
    r.capacity = 100;
    r.orders.push_back(O(1, 106, 5, 10, 1));
    TripPlan p = planTrip(r);
    CHECK(p.selected.size() == 1);
    CHECK(p.distance == 0);
    CHECK(p.route.size() == 1);
    CHECK(p.stops.size() == 1 && p.stops[0].node == 0);

    r.orders.push_back(O(2, 102, 5, 10, 2));             // plus one real stop
    TripPlan q = planTrip(r);
    CHECK(q.stops.size() == 2 && q.stops[0].node == 0 && q.stops[1].node == 6);
    CHECK(q.distance == 18);
    CHECK(routeIsValid(r, q));
}

void test_duplicate_sku_in_inventory_uses_latest_location() {
    TripRequest r = warehouseRequest();
    r.inventory.push_back(S(102, 7));                    // SKU 102 moved from node 6 to node 7
    r.capacity = 100;
    r.orders.push_back(O(1, 102, 5, 10, 1));
    TripPlan p = planTrip(r);
    CHECK(p.stops.size() == 1 && p.stops[0].node == 7);
    CHECK(p.distance == 26);                             // 0 -> 7 is 13 each way
}

static bool throwsInvalid(const TripRequest& r) {
    try { planTrip(r); } catch (const std::invalid_argument&) { return true; }
    return false;
}

void test_input_validation() {
    TripRequest base = warehouseRequest();
    base.capacity = 50;

    TripRequest r = base; r.nodes = 0;                   CHECK(throwsInvalid(r));
    r = base; r.depot = 8;                               CHECK(throwsInvalid(r));
    r = base; r.depot = -1;                              CHECK(throwsInvalid(r));
    r = base; r.capacity = -1;                           CHECK(throwsInvalid(r));
    r = base; r.capacity = PLAN_MAX_CAPACITY + 1;        CHECK(throwsInvalid(r));
    r = base; r.maxCandidates = 0;                       CHECK(throwsInvalid(r));
    r = base; r.maxCandidates = PLAN_MAX_CANDIDATES + 1; CHECK(throwsInvalid(r));
    r = base; r.edges.push_back(E(0, 50, 1));            CHECK(throwsInvalid(r));
    r = base; r.edges.push_back(E(0, 1, -3));            CHECK(throwsInvalid(r));
    r = base; r.inventory.push_back(S(7, 99));           CHECK(throwsInvalid(r));
    r = base; r.orders.push_back(O(1, 101, -5, 10, 1));  CHECK(throwsInvalid(r));
    r = base; r.orders.push_back(O(1, 101, 5, -10, 1));  CHECK(throwsInvalid(r));
    CHECK(!throwsInvalid(base));                         // and the unmodified request is fine
}

// ---------- independent oracle ----------
// Shortest distances by Floyd-Warshall (a different algorithm from Dijkstra),
// and the best tour by trying every permutation of the stops.
static std::vector<std::vector<long long> > floydWarshall(const TripRequest& r) {
    const long long INF = 1LL << 50;
    std::vector<std::vector<long long> > d(r.nodes, std::vector<long long>(r.nodes, INF));
    for (int i = 0; i < r.nodes; ++i) d[i][i] = 0;
    for (std::size_t i = 0; i < r.edges.size(); ++i) {
        d[r.edges[i].u][r.edges[i].v] = std::min<long long>(d[r.edges[i].u][r.edges[i].v], r.edges[i].w);
        d[r.edges[i].v][r.edges[i].u] = std::min<long long>(d[r.edges[i].v][r.edges[i].u], r.edges[i].w);
    }
    for (int k = 0; k < r.nodes; ++k)
        for (int i = 0; i < r.nodes; ++i)
            for (int j = 0; j < r.nodes; ++j)
                if (d[i][k] + d[k][j] < d[i][j]) d[i][j] = d[i][k] + d[k][j];
    return d;
}

static long long bruteForceTour(const std::vector<std::vector<long long> >& d, int depot, std::vector<int> stops) {
    if (stops.empty()) return 0;
    std::sort(stops.begin(), stops.end());
    long long best = -1;
    do {
        long long total = d[depot][stops.front()];
        for (std::size_t i = 0; i + 1 < stops.size(); ++i) total += d[stops[i]][stops[i + 1]];
        total += d[stops.back()][depot];
        if (best < 0 || total < best) best = total;
    } while (std::next_permutation(stops.begin(), stops.end()));
    return best;
}

static long long bruteForceBestValue(const std::vector<PlanOrder>& pool, int capacity) {
    long long best = 0;
    const int n = static_cast<int>(pool.size());
    for (int mask = 0; mask < (1 << n); ++mask) {
        long long w = 0, v = 0;
        for (int i = 0; i < n; ++i) if (mask & (1 << i)) { w += pool[i].weight; v += pool[i].value; }
        if (w <= capacity && v > best) best = v;
    }
    return best;
}

void test_random_plans_against_the_oracle() {
    std::mt19937 rng(2026);
    bool valueOptimal = true, tourOptimal = true, routesValid = true, poolCorrect = true,
         partitionOk = true, weightOk = true;
    int plansWithStops = 0;
    for (int trial = 0; trial < 250; ++trial) {
        TripRequest r;
        r.nodes = 4 + static_cast<int>(rng() % 9);                       // 4..12 nodes
        // a random spanning chain keeps most of the floor connected, extra edges add shortcuts
        for (int v = 1; v < r.nodes; ++v)
            if (rng() % 10 != 0) r.edges.push_back(E(static_cast<int>(rng() % v), v, 1 + static_cast<int>(rng() % 20)));
        int extra = static_cast<int>(rng() % (r.nodes + 1));
        for (int i = 0; i < extra; ++i)
            r.edges.push_back(E(static_cast<int>(rng() % r.nodes), static_cast<int>(rng() % r.nodes), 1 + static_cast<int>(rng() % 25)));
        r.depot = static_cast<int>(rng() % r.nodes);
        r.capacity = static_cast<int>(rng() % 60);
        r.maxCandidates = 1 + static_cast<int>(rng() % 7);
        const int skuCount = 2 + static_cast<int>(rng() % 10);
        for (int s = 0; s < skuCount; ++s) r.inventory.push_back(S(1000 + s, static_cast<int>(rng() % r.nodes)));
        const int orderCount = static_cast<int>(rng() % 12);
        for (int i = 0; i < orderCount; ++i)
            r.orders.push_back(O(500 + i, 1000 + static_cast<int>(rng() % (skuCount + 2)),   // some SKUs are unknown
                                 static_cast<int>(rng() % 25), static_cast<int>(rng() % 100), static_cast<int>(rng() % 6)));

        TripPlan p = planTrip(r);
        std::vector<std::vector<long long> > d = floydWarshall(r);

        // Oracle for the candidate pool: valid orders, sorted by (urgency, arrival), first maxCandidates.
        std::map<int, int> loc;
        for (std::size_t i = 0; i < r.inventory.size(); ++i) loc[r.inventory[i].sku] = r.inventory[i].node;
        std::vector<std::pair<std::pair<int, std::size_t>, std::size_t> > valid;   // ((urgency, arrival), index)
        for (std::size_t i = 0; i < r.orders.size(); ++i) {
            std::map<int, int>::iterator it = loc.find(r.orders[i].sku);
            if (it == loc.end()) continue;
            if (d[it->second][r.depot] >= (1LL << 50)) continue;              // unreachable
            valid.push_back(std::make_pair(std::make_pair(r.orders[i].urgency, i), i));
        }
        std::sort(valid.begin(), valid.end());
        std::vector<PlanOrder> pool;
        std::set<long long> poolIds;
        for (std::size_t k = 0; k < valid.size() && static_cast<int>(k) < r.maxCandidates; ++k) {
            pool.push_back(r.orders[valid[k].second]);
            poolIds.insert(r.orders[valid[k].second].id);
        }
        if (p.candidates != static_cast<int>(pool.size())) poolCorrect = false;

        // The plan must be an optimal knapsack choice from that pool.
        if (p.totalValue != bruteForceBestValue(pool, r.capacity)) valueOptimal = false;

        // Selected orders come from the pool, fit the payload, and add up to the reported value.
        long long w = 0, v = 0;
        std::vector<int> stopNodes;
        for (std::size_t k = 0; k < p.selected.size(); ++k) {
            bool found = false;
            for (std::size_t i = 0; i < r.orders.size(); ++i) {
                if (r.orders[i].id != p.selected[k]) continue;
                found = true;
                w += r.orders[i].weight; v += r.orders[i].value;
                int node = loc[r.orders[i].sku];
                if (node != r.depot && std::find(stopNodes.begin(), stopNodes.end(), node) == stopNodes.end())
                    stopNodes.push_back(node);
            }
            if (!found || poolIds.find(p.selected[k]) == poolIds.end()) poolCorrect = false;
        }
        if (w > r.capacity || w != p.totalWeight || v != p.totalValue) weightOk = false;

        // selected + deferred + rejected account for every order exactly once.
        if (p.selected.size() + p.deferred.size() + p.rejected.size() != r.orders.size()) partitionOk = false;

        // The route must be valid and its length must match the true optimal tour over those stops.
        if (!routeIsValid(r, p)) routesValid = false;
        if (p.distance != bruteForceTour(d, r.depot, stopNodes)) tourOptimal = false;
        if (!stopNodes.empty()) ++plansWithStops;
    }
    CHECK(valueOptimal);
    CHECK(tourOptimal);
    CHECK(routesValid);
    CHECK(poolCorrect);
    CHECK(partitionOk);
    CHECK(weightOk);
    CHECK(plansWithStops > 100);                         // the random cases really exercised routing
}

// ---------- benchmark ----------

void benchmark() {
    std::cout << "\nBenchmark: one full plan_trip on a large warehouse\n";
    struct Case { int side; int skus; int orders; };
    Case cases[3] = { {30, 10000, 500}, {100, 100000, 5000}, {300, 1000000, 20000} };
    for (int c = 0; c < 3; ++c) {
        const int side = cases[c].side;                  // floor is a side x side grid of aisle junctions
        TripRequest r;
        r.nodes = side * side;
        std::mt19937 rng(9);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                int id = y * side + x;
                if (x + 1 < side) r.edges.push_back(E(id, id + 1, 1 + static_cast<int>(rng() % 5)));
                if (y + 1 < side) r.edges.push_back(E(id, id + side, 1 + static_cast<int>(rng() % 5)));
            }
        }
        r.depot = 0;
        r.capacity = 500;
        r.maxCandidates = 12;
        for (int s = 0; s < cases[c].skus; ++s) r.inventory.push_back(S(s, static_cast<int>(rng() % r.nodes)));
        for (int i = 0; i < cases[c].orders; ++i)
            r.orders.push_back(O(i, static_cast<int>(rng() % cases[c].skus), 5 + static_cast<int>(rng() % 60),
                                 1 + static_cast<int>(rng() % 100), static_cast<int>(rng() % 10)));
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        TripPlan p = planTrip(r);
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
        std::cout << "  floor " << r.nodes << " nodes, " << cases[c].skus << " SKUs, " << cases[c].orders
                  << " orders  ->  " << ms << " ms;  trip: " << p.selected.size() << " orders, "
                  << p.stops.size() << " stops, distance " << p.distance << ", value " << p.totalValue
                  << ", B&B explored " << p.boundNodesExplored << " nodes\n";
    }
}

int main() {
    test_classic_knapsack_through_the_pipeline();
    test_everything_fits();
    test_zero_capacity_and_no_orders();
    test_rejected_orders();
    test_heap_limits_the_candidate_pool();
    test_equal_urgency_is_first_come_first_served();
    test_orders_sharing_a_shelf_make_one_stop();
    test_orders_at_the_depot_cost_nothing();
    test_duplicate_sku_in_inventory_uses_latest_location();
    test_input_validation();
    test_random_plans_against_the_oracle();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
