// test_mst.cpp - correctness tests + benchmark for Kruskal and Prim.
// Build:  g++ -std=c++11 -O2 test_mst.cpp -o test_mst
// Run:    .\test_mst.exe

#include "mst.hpp"
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

// Is `r` a genuine spanning forest of g? Checks: every chosen edge exists in g,
// no cycles, the right number of edges, and the weights add up.
static bool validForest(const EdgeGraph& g, const MstResult& r) {
    const int n = g.size();
    DisjointSets all(n);
    for (std::size_t i = 0; i < g.edges().size(); ++i) all.unite(g.edges()[i].u, g.edges()[i].v);
    if (r.components != all.groups()) return false;
    if (static_cast<int>(r.edges.size()) != n - all.groups()) return false;

    DisjointSets chosen(n);
    long long sum = 0;
    for (std::size_t i = 0; i < r.edges.size(); ++i) {
        const MstEdge& e = r.edges[i];
        bool exists = false;
        for (std::size_t j = 0; j < g.edges().size() && !exists; ++j) {
            const MstEdge& f = g.edges()[j];
            exists = f.w == e.w && ((f.u == e.u && f.v == e.v) || (f.u == e.v && f.v == e.u));
        }
        if (!exists) return false;
        if (!chosen.unite(e.u, e.v)) return false;          // would close a cycle
        sum += e.w;
    }
    return sum == r.totalWeight && r.spanning == (all.groups() == 1);
}

// Textbook graph (CLRS, nodes a..i = 0..8). The minimum spanning tree weighs 37.
static EdgeGraph textbook() {
    EdgeGraph g(9);
    g.addEdge(0, 1, 4);  g.addEdge(0, 7, 8);  g.addEdge(1, 2, 8);  g.addEdge(1, 7, 11);
    g.addEdge(2, 3, 7);  g.addEdge(2, 5, 4);  g.addEdge(2, 8, 2);  g.addEdge(3, 4, 9);
    g.addEdge(3, 5, 14); g.addEdge(4, 5, 10); g.addEdge(5, 6, 2);  g.addEdge(6, 7, 1);
    g.addEdge(6, 8, 6);  g.addEdge(7, 8, 7);
    return g;
}

// The 8-node warehouse floor used in the Dijkstra / Branch and Bound tests.
// Work Kruskal out by hand: take 0-4(2), 5-6(2), 1-2(3), 1-5(3), 0-1(4), 6-7(4),
// skip 2-6(4) (would form a cycle), take 2-3(5). Seven edges, total 23.
static EdgeGraph warehouseFloor() {
    EdgeGraph g(8);
    g.addEdge(0, 1, 4); g.addEdge(1, 2, 3); g.addEdge(2, 3, 5); g.addEdge(0, 4, 2);
    g.addEdge(4, 5, 6); g.addEdge(5, 6, 2); g.addEdge(6, 7, 4); g.addEdge(1, 5, 3);
    g.addEdge(3, 7, 6); g.addEdge(2, 6, 4);
    return g;
}

void test_textbook() {
    EdgeGraph g = textbook();
    MstResult k = g.kruskal(), p = g.prim();
    CHECK(k.spanning && p.spanning);
    CHECK(k.totalWeight == 37);
    CHECK(p.totalWeight == 37);
    CHECK(k.edges.size() == 8 && p.edges.size() == 8);
    CHECK(validForest(g, k));
    CHECK(validForest(g, p));
}

void test_warehouse_floor() {
    EdgeGraph g = warehouseFloor();
    MstResult k = g.kruskal(), p = g.prim();
    CHECK(k.totalWeight == 23);
    CHECK(p.totalWeight == 23);
    CHECK(k.edges.size() == 7);
    CHECK(validForest(g, k));
    CHECK(validForest(g, p));
}

void test_disconnected_gives_forest() {
    EdgeGraph g(6);                  // pieces: {0,1,2}, {3,4}, {5}
    g.addEdge(0, 1, 5); g.addEdge(1, 2, 3); g.addEdge(0, 2, 9);
    g.addEdge(3, 4, 7);
    MstResult k = g.kruskal(), p = g.prim();
    CHECK(!k.spanning && !p.spanning);
    CHECK(k.components == 3 && p.components == 3);
    CHECK(k.totalWeight == 3 + 5 + 7);
    CHECK(p.totalWeight == 15);
    CHECK(k.edges.size() == 3);       // n - components = 6 - 3
    CHECK(validForest(g, k));
    CHECK(validForest(g, p));
}

void test_tiny_graphs() {
    EdgeGraph one(1);                 // a single node: already a tree
    MstResult k = one.kruskal(), p = one.prim();
    CHECK(k.spanning && p.spanning);
    CHECK(k.totalWeight == 0 && k.edges.empty());

    EdgeGraph none(3);                // three nodes, no edges at all
    MstResult k2 = none.kruskal();
    CHECK(!k2.spanning && k2.components == 3 && k2.edges.empty());
    CHECK(none.prim().components == 3);
}

void test_negative_weights() {
    EdgeGraph g(3);
    g.addEdge(0, 1, -1); g.addEdge(1, 2, -2); g.addEdge(0, 2, 3);
    CHECK(g.kruskal().totalWeight == -3);
    CHECK(g.prim().totalWeight == -3);
}

void test_self_loops_and_parallel_edges() {
    EdgeGraph g(3);
    g.addEdge(0, 0, 1);               // self-loop: can never be used
    g.addEdge(0, 1, 10);
    g.addEdge(0, 1, 4);               // cheaper parallel edge should win
    g.addEdge(1, 2, 6);
    MstResult k = g.kruskal(), p = g.prim();
    CHECK(k.totalWeight == 10 && p.totalWeight == 10);
    CHECK(validForest(g, k));
    CHECK(validForest(g, p));
}

void test_bad_input() {
    bool threwSize = false, threwRange = false;
    try { EdgeGraph g(0); } catch (const std::invalid_argument&) { threwSize = true; }
    EdgeGraph g(3);
    try { g.addEdge(0, 5, 1); } catch (const std::out_of_range&) { threwRange = true; }
    CHECK(threwSize);
    CHECK(threwRange);
}

// Cheapest spanning forest by trying every subset of edges (only for tiny graphs).
static long long bruteForceMst(const EdgeGraph& g) {
    const int n = g.size();
    const int m = static_cast<int>(g.edges().size());
    DisjointSets all(n);
    for (int i = 0; i < m; ++i) all.unite(g.edges()[i].u, g.edges()[i].v);
    const int need = n - all.groups();

    long long best = -1;
    bool have = false;
    for (long long mask = 0; mask < (1LL << m); ++mask) {
        int count = 0;
        for (int i = 0; i < m; ++i) if (mask & (1LL << i)) ++count;
        if (count != need) continue;
        DisjointSets d(n);
        long long w = 0;
        bool ok = true;
        for (int i = 0; i < m && ok; ++i) {
            if (!(mask & (1LL << i))) continue;
            if (!d.unite(g.edges()[i].u, g.edges()[i].v)) ok = false;   // cycle
            w += g.edges()[i].w;
        }
        if (ok && (!have || w < best)) { best = w; have = true; }
    }
    return best;
}

void test_random_against_brute_force() {
    std::mt19937 rng(8);
    bool weightsOk = true, forestsOk = true;
    for (int trial = 0; trial < 300; ++trial) {
        int n = 1 + static_cast<int>(rng() % 7);               // 1..7 nodes
        int m = static_cast<int>(rng() % 13);                  // 0..12 edges (some graphs disconnected)
        EdgeGraph g(n);
        for (int i = 0; i < m; ++i)
            g.addEdge(static_cast<int>(rng() % n), static_cast<int>(rng() % n),
                      static_cast<long long>(rng() % 41) - 10);   // weights -10..30, many ties
        MstResult k = g.kruskal(), p = g.prim();
        long long best = bruteForceMst(g);
        if (k.totalWeight != best || p.totalWeight != best) weightsOk = false;
        if (!validForest(g, k) || !validForest(g, p)) forestsOk = false;
    }
    CHECK(weightsOk);
    CHECK(forestsOk);
}

void test_kruskal_and_prim_agree_on_bigger_graphs() {
    std::mt19937 rng(21);
    bool agree = true, valid = true;
    for (int trial = 0; trial < 30; ++trial) {
        int n = 50 + static_cast<int>(rng() % 150);
        int m = n + static_cast<int>(rng() % (4 * n));
        EdgeGraph g(n);
        for (int i = 0; i < m; ++i)
            g.addEdge(static_cast<int>(rng() % n), static_cast<int>(rng() % n),
                      1 + static_cast<long long>(rng() % 100));
        MstResult k = g.kruskal(), p = g.prim();
        if (k.totalWeight != p.totalWeight || k.components != p.components) agree = false;
        if (!validForest(g, k) || !validForest(g, p)) valid = false;
    }
    CHECK(agree);
    CHECK(valid);
}

// ---------- benchmark ----------

static long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

// Connected random graph: a random spanning chain first, then extra random edges.
static EdgeGraph randomConnected(int n, long long edges, unsigned seed) {
    std::mt19937 rng(seed);
    EdgeGraph g(n);
    for (int i = 1; i < n; ++i)
        g.addEdge(i - 1, i, 1 + static_cast<long long>(rng() % 1000));
    for (long long i = n - 1; i < edges; ++i)
        g.addEdge(static_cast<int>(rng() % n), static_cast<int>(rng() % n),
                  1 + static_cast<long long>(rng() % 1000));
    return g;
}

void benchmark() {
    std::cout << "\nBenchmark: Kruskal vs Prim on random connected graphs\n";
    struct Case { int n; long long m; const char* label; };
    Case cases[4] = {
        {10000, 50000, "sparse"},
        {100000, 500000, "sparse"},
        {1000000, 3000000, "sparse"},
        {2000, 1000000, "dense "}
    };
    for (int c = 0; c < 4; ++c) {
        EdgeGraph g = randomConnected(cases[c].n, cases[c].m, 4);
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        MstResult k = g.kruskal();
        long long kMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        MstResult p = g.prim();
        long long pMs = msSince(t0);
        std::cout << "  " << cases[c].label << "  V = " << cases[c].n << ", E = " << cases[c].m
                  << "  Kruskal: " << kMs << " ms   Prim: " << pMs << " ms   same weight: "
                  << (k.totalWeight == p.totalWeight ? "yes" : "NO")
                  << "  (MST weight " << k.totalWeight << ")\n";
    }
}

int main() {
    test_textbook();
    test_warehouse_floor();
    test_disconnected_gives_forest();
    test_tiny_graphs();
    test_negative_weights();
    test_self_loops_and_parallel_edges();
    test_bad_input();
    test_random_against_brute_force();
    test_kruskal_and_prim_agree_on_bigger_graphs();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
