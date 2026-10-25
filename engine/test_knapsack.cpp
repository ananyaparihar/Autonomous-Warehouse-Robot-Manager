// test_knapsack.cpp - correctness tests + benchmark.
// Build:  g++ -std=c++11 -O2 test_knapsack.cpp -o test_knapsack
// Run:    .\test_knapsack.exe

#include "knapsack.hpp"
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

// Try every subset (2^n). Only usable for small n; used to check the DP.
static int bruteForceBest(const std::vector<Item>& items, int capacity) {
    const int n = static_cast<int>(items.size());
    int best = 0;
    for (long long mask = 0; mask < (1LL << n); ++mask) {
        int w = 0, v = 0;
        for (int i = 0; i < n; ++i) {
            if (mask & (1LL << i)) { w += items[i].weight; v += items[i].value; }
        }
        if (w <= capacity && v > best) best = v;
    }
    return best;
}

// Classic example where greedy-by-ratio fails.
// Capacity 50. Items (weight, value): (10, 60), (20, 100), (30, 120).
// Ratios 6, 5, 4. Greedy takes items 1+2 = 160. Best is items 2+3 = 220.
static std::vector<Item> classic() {
    std::vector<Item> items;
    items.push_back({1, 10, 60});
    items.push_back({2, 20, 100});
    items.push_back({3, 30, 120});
    return items;
}

void test_classic() {
    KnapsackResult dp = knapsackDP(classic(), 50);
    CHECK(dp.totalValue == 220);
    CHECK(dp.totalWeight == 50);
    CHECK((dp.chosen == std::vector<int>{2, 3}));
}

void test_greedy_is_worse_here() {
    KnapsackResult g = knapsackGreedy(classic(), 50);
    CHECK(g.totalValue == 160);          // greedy misses the optimum
    CHECK(g.totalValue < 220);
}

void test_edge_cases() {
    std::vector<Item> none;
    KnapsackResult r0 = knapsackDP(none, 10);
    CHECK(r0.totalValue == 0 && r0.chosen.empty());

    KnapsackResult r1 = knapsackDP(classic(), 0);          // zero capacity
    CHECK(r1.totalValue == 0 && r1.chosen.empty());

    KnapsackResult r2 = knapsackDP(classic(), 5);          // nothing fits
    CHECK(r2.totalValue == 0 && r2.chosen.empty());

    KnapsackResult r3 = knapsackDP(classic(), 1000);       // everything fits
    CHECK(r3.totalValue == 280);
    CHECK(r3.chosen.size() == 3);
}

void test_bad_input() {
    bool threwCap = false, threwItem = false;
    try { knapsackDP(classic(), -1); } catch (const std::invalid_argument&) { threwCap = true; }
    std::vector<Item> bad;
    bad.push_back({1, -3, 10});
    try { knapsackDP(bad, 10); } catch (const std::invalid_argument&) { threwItem = true; }
    CHECK(threwCap);
    CHECK(threwItem);
}

// 300 random small cases: DP value must equal brute force, the chosen set must
// really fit, and its values must add up. Greedy must never beat the DP.
void test_random_against_brute_force() {
    std::mt19937 rng(2026);
    bool allOk = true, greedyNeverBetter = true;
    for (int trial = 0; trial < 300; ++trial) {
        int n = 1 + static_cast<int>(rng() % 12);
        int cap = static_cast<int>(rng() % 60);
        std::vector<Item> items;
        for (int i = 0; i < n; ++i)
            items.push_back({i, static_cast<int>(rng() % 25), static_cast<int>(rng() % 100)});

        KnapsackResult dp = knapsackDP(items, cap);
        int best = bruteForceBest(items, cap);

        int sumW = 0, sumV = 0;
        for (std::size_t k = 0; k < dp.chosen.size(); ++k) {
            sumW += items[dp.chosen[k]].weight;
            sumV += items[dp.chosen[k]].value;
        }
        if (dp.totalValue != best || sumW > cap || sumV != dp.totalValue || sumW != dp.totalWeight)
            allOk = false;
        if (knapsackGreedy(items, cap).totalValue > dp.totalValue) greedyNeverBetter = false;
    }
    CHECK(allOk);
    CHECK(greedyNeverBetter);
}

static std::vector<Item> randomItems(int n, int maxW, int maxV, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Item> items;
    for (int i = 0; i < n; ++i)
        items.push_back({i, 1 + static_cast<int>(rng() % maxW), 1 + static_cast<int>(rng() % maxV)});
    return items;
}

static long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

void benchmark() {
    std::cout << "\nBenchmark 1: DP time grows with n * W\n";
    int ns[3] = {100, 500, 1000};
    int ws[3] = {1000, 10000, 20000};
    for (int k = 0; k < 3; ++k) {
        std::vector<Item> items = randomItems(ns[k], 100, 1000, 5);
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        KnapsackResult dp = knapsackDP(items, ws[k]);
        long long dpMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        KnapsackResult g = knapsackGreedy(items, ws[k]);
        long long gMs = msSince(t0);
        double gap = dp.totalValue == 0 ? 0.0
                     : 100.0 * (dp.totalValue - g.totalValue) / dp.totalValue;
        std::cout << "  n = " << ns[k] << ", W = " << ws[k]
                  << "  DP: " << dpMs << " ms (value " << dp.totalValue << ")"
                  << "  greedy: " << gMs << " ms (value " << g.totalValue << ", "
                  << gap << "% below optimal)\n";
    }

    std::cout << "\nBenchmark 2: DP vs brute force (2^n)\n";
    int smallNs[3] = {16, 20, 24};
    for (int k = 0; k < 3; ++k) {
        std::vector<Item> items = randomItems(smallNs[k], 30, 100, 9);
        int cap = 150;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        KnapsackResult dp = knapsackDP(items, cap);
        long long dpMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        int best = bruteForceBest(items, cap);
        long long bfMs = msSince(t0);
        std::cout << "  n = " << smallNs[k] << "  DP: " << dpMs << " ms   brute force: "
                  << bfMs << " ms   same answer: " << (best == dp.totalValue ? "yes" : "NO") << "\n";
    }
}

int main() {
    test_classic();
    test_greedy_is_worse_here();
    test_edge_cases();
    test_bad_input();
    test_random_against_brute_force();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
