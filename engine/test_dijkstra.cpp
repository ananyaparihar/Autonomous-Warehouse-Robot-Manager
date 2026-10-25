// test_dijkstra.cpp - correctness tests + a small benchmark.
// Build:  g++ -std=c++17 -O2 test_dijkstra.cpp -o test_dijkstra
// Run:    .\test_dijkstra.exe

#include "dijkstra.hpp"
#include <chrono>
#include <iostream>
#include <random>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

// Small graph you can verify by hand:
//
//        (4)       (1)
//   0 -------- 1 -------- 2
//   |          |          |
//  (1)        (2)        (6)
//   |          |          |
//   3 -------- 4 -------- 5
//        (3)        (1)
//
// Shortest 0 -> 5 is 0-3-4-5 = 1 + 3 + 1 = 5 (3 edges),
// shorter than 0-1-2-5 = 4 + 1 + 6 = 11.
Graph makeSmall() {
    Graph g(6);
    g.addEdge(0, 1, 4);
    g.addEdge(1, 2, 1);
    g.addEdge(0, 3, 1);
    g.addEdge(1, 4, 2);
    g.addEdge(2, 5, 6);
    g.addEdge(3, 4, 3);
    g.addEdge(4, 5, 1);
    return g;
}

void test_small_graph() {
    Graph g = makeSmall();
    PathResult r = g.shortestPath(0, 5);
    CHECK(r.reachable);
    CHECK(r.distance == 5);
    CHECK((r.path == std::vector<int>{0, 3, 4, 5}));
}

void test_fewest_edges_is_not_shortest() {
    // Direct edge 0-2 costs 10, but 0-1-2 costs 2.
    Graph g(3);
    g.addEdge(0, 2, 10);
    g.addEdge(0, 1, 1);
    g.addEdge(1, 2, 1);
    PathResult r = g.shortestPath(0, 2);
    CHECK(r.distance == 2);
    CHECK((r.path == std::vector<int>{0, 1, 2}));
}

void test_disconnected() {
    Graph g(4);
    g.addEdge(0, 1, 1);
    g.addEdge(2, 3, 1);   // separate component
    PathResult r = g.shortestPath(0, 3);
    CHECK(!r.reachable);
    CHECK(r.distance == -1);
    CHECK(r.path.empty());
}

void test_source_equals_target() {
    Graph g = makeSmall();
    PathResult r = g.shortestPath(2, 2);
    CHECK(r.reachable);
    CHECK(r.distance == 0);
    CHECK((r.path == std::vector<int>{2}));
}

void test_bad_input() {
    Graph g(3);
    bool threwNeg = false, threwRange = false;
    try { g.addEdge(0, 1, -5); } catch (const std::invalid_argument&) { threwNeg = true; }
    try { g.shortestPath(0, 9); } catch (const std::out_of_range&) { threwRange = true; }
    CHECK(threwNeg);
    CHECK(threwRange);
}

void benchmark() {
    std::cout << "\nBenchmark (random graph, 5 edges per node, one query):\n";
    for (int n : {10000, 100000, 1000000}) {
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> node(0, n - 1);
        std::uniform_int_distribution<int> weight(1, 100);
        Graph g(n);
        for (int i = 0; i < n * 5; ++i) g.addEdge(node(rng), node(rng), weight(rng));
        auto t0 = std::chrono::steady_clock::now();
        PathResult r = g.shortestPath(0, n - 1);
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  V = " << n << ", E = " << n * 5 << "  ->  " << ms << " ms"
                  << (r.reachable ? "" : "  (target unreachable)") << "\n";
    }
}

int main() {
    test_small_graph();
    test_fewest_edges_is_not_shortest();
    test_disconnected();
    test_source_equals_target();
    test_bad_input();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
