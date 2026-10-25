// test_min_heap.cpp - correctness tests + a small benchmark.
// Build:  g++ -std=c++17 -O2 test_min_heap.cpp -o test_min_heap
// Run:    ./test_min_heap        (Windows: .\test_min_heap.exe)

#include "min_heap.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

void test_basic_order() {
    MinHeap h;
    h.push({1, "restock", 5});
    h.push({2, "urgent pick", 1});
    h.push({3, "normal pick", 3});
    CHECK(h.size() == 3);
    CHECK(h.top().id == 2);
    CHECK(h.pop().id == 2);
    CHECK(h.pop().id == 3);
    CHECK(h.pop().id == 1);
    CHECK(h.empty());
}

void test_ties_are_fifo() {
    MinHeap h;
    h.push({10, "a", 2});
    h.push({11, "b", 2});
    h.push({12, "c", 2});
    CHECK(h.pop().id == 10);
    CHECK(h.pop().id == 11);
    CHECK(h.pop().id == 12);
}

void test_empty_throws() {
    MinHeap h;
    bool threw = false;
    try { h.pop(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void test_random_matches_sort() {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, 1000);
    MinHeap h;
    std::vector<int> expected;
    for (int i = 0; i < 5000; ++i) {
        int u = dist(rng);
        h.push({i, "t", u});
        expected.push_back(u);
    }
    std::sort(expected.begin(), expected.end());
    bool ok = true;
    for (int u : expected) {
        if (h.pop().urgency != u) { ok = false; break; }
    }
    CHECK(ok);
}

void benchmark() {
    std::cout << "\nBenchmark (push n, then pop all):\n";
    for (int n : {10000, 100000, 1000000}) {
        std::mt19937 rng(1);
        std::uniform_int_distribution<int> dist(1, 1000000);
        MinHeap h;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) h.push({i, "t", dist(rng)});
        while (!h.empty()) h.pop();
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  n = " << n << "  ->  " << ms << " ms\n";
    }
}

int main() {
    test_basic_order();
    test_ties_are_fifo();
    test_empty_throws();
    test_random_matches_sort();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
