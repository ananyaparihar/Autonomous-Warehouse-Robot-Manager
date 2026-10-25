// test_branch_bound.cpp - correctness tests + benchmark.
// Needs dijkstra.hpp in the same folder (used for the integration test).
// Build:  g++ -std=c++11 -O2 test_branch_bound.cpp -o test_branch_bound
// Run:    .\test_branch_bound.exe

#include "branch_bound.hpp"
#include "dijkstra.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

// Is `route` a valid tour (starts and ends at 0, visits every stop once)
// and does its length equal `expectedCost`?
static bool validTour(const DistMatrix& d, const std::vector<int>& route, long long expectedCost) {
    const int n = static_cast<int>(d.size());
    if (static_cast<int>(route.size()) != n + 1) return false;
    if (route.front() != 0 || route.back() != 0) return false;
    std::vector<int> count(n, 0);
    long long cost = 0;
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        ++count[route[i]];
        cost += d[route[i]][route[i + 1]];
    }
    for (int v = 0; v < n; ++v) if (count[v] != 1) return false;
    return cost == expectedCost;
}

// Classic 4-node example. Best tour is 0-1-3-2-0 = 10 + 25 + 30 + 15 = 80.
static DistMatrix classic4() {
    long long m[4][4] = {
        { 0, 10, 15, 20},
        {10,  0, 35, 25},
        {15, 35,  0, 30},
        {20, 25, 30,  0}
    };
    DistMatrix d(4, std::vector<long long>(4));
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) d[i][j] = m[i][j];
    return d;
}

void test_classic() {
    DistMatrix d = classic4();
    RoutePlanner planner(d);
    RouteResult r = planner.solve(true);
    CHECK(r.found);
    CHECK(r.cost == 80);
    CHECK(validTour(d, r.route, 80));
}

void test_line_layout() {
    // Stops sit on a straight line at positions 0,1,2,3,4. Distance = |i - j|.
    // Best tour goes out to 4 and comes back: cost 8.
    const int n = 5;
    DistMatrix d(n, std::vector<long long>(n));
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) d[i][j] = std::abs(i - j);
    RouteResult r = RoutePlanner(d).solve(true);
    CHECK(r.cost == 8);
    CHECK(validTour(d, r.route, 8));
}

void test_tiny_cases() {
    DistMatrix one(1, std::vector<long long>(1, 0));          // only the depot
    RouteResult r0 = RoutePlanner(one).solve(true);
    CHECK(r0.found && r0.cost == 0);

    DistMatrix two(2, std::vector<long long>(2, 0));          // depot + one stop
    two[0][1] = 7; two[1][0] = 7;
    RouteResult r1 = RoutePlanner(two).solve(true);
    CHECK(r1.found && r1.cost == 14);
    CHECK((r1.route == std::vector<int>{0, 1, 0}));
}

void test_unreachable_stop() {
    DistMatrix d = classic4();
    for (int j = 0; j < 4; ++j) { if (j != 3) { d[3][j] = ROUTE_INF; d[j][3] = ROUTE_INF; } }
    RouteResult r = RoutePlanner(d).solve(true);
    CHECK(!r.found);
    CHECK(r.cost == -1);
    CHECK(r.route.empty());
}

void test_bad_input() {
    bool threwSquare = false, threwNeg = false, threwEmpty = false;
    DistMatrix ragged(2, std::vector<long long>(3, 1));
    try { RoutePlanner p(ragged); } catch (const std::invalid_argument&) { threwSquare = true; }
    DistMatrix neg = classic4();
    neg[1][2] = -4;
    try { RoutePlanner p(neg); } catch (const std::invalid_argument&) { threwNeg = true; }
    DistMatrix empty;
    try { RoutePlanner p(empty); } catch (const std::invalid_argument&) { threwEmpty = true; }
    CHECK(threwSquare);
    CHECK(threwNeg);
    CHECK(threwEmpty);
}

// 300 random matrices (asymmetric, so the bound is tested on the hard case).
// Branch and Bound must give the same cost as brute force.
void test_random_against_brute_force() {
    std::mt19937 rng(11);
    bool sameCost = true, toursValid = true, neverMoreNodes = true;
    for (int trial = 0; trial < 300; ++trial) {
        int n = 2 + static_cast<int>(rng() % 8);          // 2..9 nodes
        DistMatrix d(n, std::vector<long long>(n, 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (i != j) d[i][j] = 1 + static_cast<long long>(rng() % 50);
        RouteResult bb = RoutePlanner(d).solve(true);
        RouteResult bf = RoutePlanner(d).solve(false);
        if (!bb.found || !bf.found || bb.cost != bf.cost) sameCost = false;
        if (bb.found && !validTour(d, bb.route, bb.cost)) toursValid = false;
        if (bb.nodesExplored > bf.nodesExplored) neverMoreNodes = false;
    }
    CHECK(sameCost);
    CHECK(toursValid);
    CHECK(neverMoreNodes);
}

// Integration: floor graph (Dijkstra) -> distance matrix -> best pick route.
void test_integration_with_dijkstra() {
    // 8-node warehouse floor. Node 0 = depot, the rest are aisle junctions.
    Graph floor(8);
    floor.addEdge(0, 1, 4);
    floor.addEdge(1, 2, 3);
    floor.addEdge(2, 3, 5);
    floor.addEdge(0, 4, 2);
    floor.addEdge(4, 5, 6);
    floor.addEdge(5, 6, 2);
    floor.addEdge(6, 7, 4);
    floor.addEdge(1, 5, 3);
    floor.addEdge(3, 7, 6);
    floor.addEdge(2, 6, 4);

    // Pick stops (depot first): shelf locations 0, 3, 6, 7, 2
    std::vector<int> stops;
    stops.push_back(0); stops.push_back(3); stops.push_back(6);
    stops.push_back(7); stops.push_back(2);

    const int k = static_cast<int>(stops.size());
    DistMatrix d(k, std::vector<long long>(k, 0));
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j)
            if (i != j) d[i][j] = floor.shortestPath(stops[i], stops[j]).distance;

    RouteResult bb = RoutePlanner(d).solve(true);
    RouteResult bf = RoutePlanner(d).solve(false);
    CHECK(bb.found);
    CHECK(bb.cost == bf.cost);
    CHECK(validTour(d, bb.route, bb.cost));
}

// ---------- benchmark ----------

static DistMatrix randomEuclidean(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<int> x(n), y(n);
    for (int i = 0; i < n; ++i) { x[i] = static_cast<int>(rng() % 100); y[i] = static_cast<int>(rng() % 100); }
    DistMatrix d(n, std::vector<long long>(n, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            d[i][j] = static_cast<long long>(std::sqrt(double((x[i]-x[j])*(x[i]-x[j]) + (y[i]-y[j])*(y[i]-y[j]))) + 0.5);
    return d;
}

static long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

void benchmark() {
    std::cout << "\nBenchmark 1: Branch and Bound vs brute force (random floor, depot + stops)\n";
    int sizes[3] = {8, 10, 11};
    for (int k = 0; k < 3; ++k) {
        DistMatrix d = randomEuclidean(sizes[k], 3);
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        RouteResult bb = RoutePlanner(d).solve(true);
        long long bbMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        RouteResult bf = RoutePlanner(d).solve(false);
        long long bfMs = msSince(t0);
        double cut = 100.0 * (1.0 - double(bb.nodesExplored) / double(bf.nodesExplored));
        std::cout << "  nodes = " << sizes[k]
                  << "  B&B: " << bbMs << " ms, " << bb.nodesExplored << " search nodes"
                  << "  |  brute force: " << bfMs << " ms, " << bf.nodesExplored << " search nodes"
                  << "  |  " << cut << "% fewer nodes, same cost: "
                  << (bb.cost == bf.cost ? "yes" : "NO") << "\n";
    }

    std::cout << "\nBenchmark 2: Branch and Bound alone on bigger routes\n";
    int bigSizes[3] = {12, 14, 16};
    for (int k = 0; k < 3; ++k) {
        DistMatrix d = randomEuclidean(bigSizes[k], 3);
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        RouteResult bb = RoutePlanner(d).solve(true);
        long long ms = msSince(t0);
        std::cout << "  nodes = " << bigSizes[k] << "  B&B: " << ms << " ms, "
                  << bb.nodesExplored << " search nodes, " << bb.nodesPruned
                  << " subtrees pruned, best cost " << bb.cost << "\n";
    }
}

int main() {
    test_classic();
    test_line_layout();
    test_tiny_cases();
    test_unreachable_stop();
    test_bad_input();
    test_random_against_brute_force();
    test_integration_with_dijkstra();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
