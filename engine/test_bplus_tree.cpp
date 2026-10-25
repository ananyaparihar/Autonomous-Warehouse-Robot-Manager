// test_bplus_tree.cpp - correctness tests + benchmark for the B+ Tree.
// Build:  g++ -std=c++11 -O2 test_bplus_tree.cpp -o test_bplus_tree
// Run:    .\test_bplus_tree.exe

#include "bplus_tree.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <random>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

typedef std::vector<std::pair<int, int> > Pairs;

void test_empty_tree() {
    BPlusTree t(4);
    int v = 0;
    CHECK(t.empty());
    CHECK(t.size() == 0);
    CHECK(!t.find(5, v));
    CHECK(t.range(0, 100).empty());
    CHECK(t.toVector().empty());
    CHECK(t.height() == 1);
    CHECK(t.validate());
}

void test_small_inserts_and_find() {
    BPlusTree t(3);
    int keys[7] = {50, 20, 70, 10, 30, 60, 80};
    for (int i = 0; i < 7; ++i) CHECK(t.insert(keys[i], keys[i] * 10));
    CHECK(t.size() == 7);
    CHECK(t.validate());
    for (int i = 0; i < 7; ++i) {
        int v = -1;
        CHECK(t.find(keys[i], v));
        CHECK(v == keys[i] * 10);
    }
    int v = -1;
    CHECK(!t.find(55, v));
    CHECK(!t.contains(0));
    CHECK(t.height() >= 2);               // 7 keys with node capacity 3 must have split
}

void test_update_existing_key() {
    BPlusTree t(4);
    CHECK(t.insert(7, 100));
    CHECK(!t.insert(7, 200));             // existing key: returns false
    CHECK(t.size() == 1);
    int v = 0;
    CHECK(t.find(7, v) && v == 200);
    CHECK(t.validate());
}

void test_negative_and_extreme_keys() {
    BPlusTree t(4);
    t.insert(-5, 1); t.insert(0, 2); t.insert(INT_MIN, 3); t.insert(INT_MAX, 4);
    int v = 0;
    CHECK(t.find(INT_MIN, v) && v == 3);
    CHECK(t.find(INT_MAX, v) && v == 4);
    CHECK(t.find(-5, v) && v == 1);
    CHECK(t.validate());
}

void test_range_basics() {
    BPlusTree t(4);
    for (int k = 0; k < 100; k += 2) t.insert(k, k + 1000);      // even keys 0..98
    Pairs r = t.range(10, 20);
    CHECK(r.size() == 6);                                         // 10,12,14,16,18,20
    CHECK(r.front().first == 10 && r.back().first == 20);
    CHECK(r.front().second == 1010);
    CHECK(t.range(11, 11).empty());                               // odd key not present
    CHECK(t.range(30, 20).empty());                               // lo > hi
    CHECK(t.range(1000, 2000).empty());                           // beyond the data
    CHECK(t.range(-50, -1).empty());                              // before the data
    CHECK(t.range(-50, 2000).size() == 50);                       // everything
    CHECK(t.range(98, 98).size() == 1);
}

// Insert in a fixed order, validating the structure after EVERY insert.
static void insertAllValidating(BPlusTree& t, const std::vector<int>& keys, bool& ok) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        t.insert(keys[i], keys[i] * 2);
        if (!t.validate()) { ok = false; return; }
    }
}

void test_orders_ascending_descending_random() {
    int orders[6] = {3, 4, 5, 6, 7, 16};
    bool ok = true, allFound = true, sorted = true;
    for (int oi = 0; oi < 6; ++oi) {
        const int m = orders[oi];
        std::vector<int> asc, desc, rnd;
        for (int i = 0; i < 400; ++i) { asc.push_back(i); desc.push_back(400 - i); rnd.push_back(i); }
        std::mt19937 rng(100 + m);
        std::shuffle(rnd.begin(), rnd.end(), rng);

        const std::vector<int>* sets[3] = {&asc, &desc, &rnd};
        for (int s = 0; s < 3; ++s) {
            BPlusTree t(m);
            insertAllValidating(t, *sets[s], ok);
            for (std::size_t i = 0; i < sets[s]->size(); ++i) {
                int v = 0;
                if (!t.find((*sets[s])[i], v) || v != (*sets[s])[i] * 2) allFound = false;
            }
            Pairs all = t.toVector();
            for (std::size_t i = 1; i < all.size(); ++i)
                if (all[i - 1].first >= all[i].first) sorted = false;
            if (static_cast<long long>(all.size()) != t.size()) sorted = false;
        }
    }
    CHECK(ok);
    CHECK(allFound);
    CHECK(sorted);
}

// Large random workload, mirrored in std::map. Both must always agree.
void test_against_std_map() {
    int orders[5] = {3, 4, 8, 32, 64};
    bool sizeOk = true, valuesOk = true, absentOk = true, rangesOk = true, validOk = true;
    for (int oi = 0; oi < 5; ++oi) {
        BPlusTree t(orders[oi]);
        std::map<int, int> ref;
        std::mt19937 rng(900 + orders[oi]);
        std::uniform_int_distribution<int> keyDist(-20000, 20000);   // duplicates will happen
        for (int i = 0; i < 20000; ++i) {
            int k = keyDist(rng), v = static_cast<int>(rng() % 100000);
            bool wasNew = (ref.find(k) == ref.end());
            if (t.insert(k, v) != wasNew) sizeOk = false;
            ref[k] = v;
        }
        if (t.size() != static_cast<long long>(ref.size())) sizeOk = false;
        if (!t.validate()) validOk = false;

        for (int k = -20100; k <= 20100; ++k) {                      // every key, present or not
            int v = 0;
            std::map<int, int>::const_iterator it = ref.find(k);
            bool found = t.find(k, v);
            if (it == ref.end()) { if (found) absentOk = false; }
            else if (!found || v != it->second) valuesOk = false;
        }

        for (int q = 0; q < 300; ++q) {                              // random range queries
            int a = keyDist(rng), b = keyDist(rng);
            if (a > b) std::swap(a, b);
            Pairs got = t.range(a, b);
            Pairs want;
            for (std::map<int, int>::const_iterator it = ref.lower_bound(a);
                 it != ref.end() && it->first <= b; ++it)
                want.push_back(*it);
            if (got != want) rangesOk = false;
        }
    }
    CHECK(sizeOk);
    CHECK(validOk);
    CHECK(valuesOk);
    CHECK(absentOk);
    CHECK(rangesOk);
}

void test_height_stays_small() {
    BPlusTree t(64);
    for (int i = 0; i < 100000; ++i) t.insert(i, i);
    CHECK(t.size() == 100000);
    CHECK(t.height() <= 5);               // a wide tree is very shallow
    CHECK(t.validate());
}

void test_bad_order() {
    bool threw = false;
    try { BPlusTree t(2); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

// ---------- delete tests ----------

void test_erase_basics() {
    BPlusTree t(4);
    CHECK(!t.erase(5));                       // erase from an empty tree
    t.insert(5, 50); t.insert(6, 60); t.insert(7, 70);
    CHECK(!t.erase(99));                      // missing key
    CHECK(t.size() == 3);
    CHECK(t.erase(6));
    CHECK(t.size() == 2);
    int v = 0;
    CHECK(!t.find(6, v));
    CHECK(t.find(5, v) && v == 50);
    CHECK(t.find(7, v) && v == 70);
    CHECK(!t.erase(6));                       // already gone
    CHECK(t.validate());
}

// Erase every key in a given order, validating after EVERY erase.
// The tree must end empty, and must work again afterwards.
void test_erase_everything() {
    int orders[6] = {3, 4, 5, 6, 7, 16};
    bool ok = true, emptyEnd = true, reusable = true, foundWhileShrinking = true;
    for (int oi = 0; oi < 6; ++oi) {
        const int m = orders[oi];
        for (int pattern = 0; pattern < 3; ++pattern) {       // ascending, descending, random
            std::vector<int> keys;
            for (int i = 0; i < 300; ++i) keys.push_back(i);
            std::vector<int> order = keys;
            if (pattern == 1) std::reverse(order.begin(), order.end());
            if (pattern == 2) { std::mt19937 rng(500 + m); std::shuffle(order.begin(), order.end(), rng); }

            BPlusTree t(m);
            for (std::size_t i = 0; i < keys.size(); ++i) t.insert(keys[i], keys[i] + 1);

            std::vector<bool> gone(300, false);
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (!t.erase(order[i])) ok = false;
                gone[order[i]] = true;
                if (!t.validate()) ok = false;
                if (i % 37 == 0) {                              // spot-check the survivors
                    for (int k = 0; k < 300; ++k) {
                        int v = 0;
                        bool f = t.find(k, v);
                        if (gone[k] == f) foundWhileShrinking = false;
                        if (f && v != k + 1) foundWhileShrinking = false;
                    }
                }
            }
            if (!t.empty() || t.height() != 1 || !t.toVector().empty()) emptyEnd = false;
            t.insert(42, 7);                                    // the emptied tree still works
            int v = 0;
            if (!t.find(42, v) || v != 7 || !t.validate()) reusable = false;
        }
    }
    CHECK(ok);
    CHECK(foundWhileShrinking);
    CHECK(emptyEnd);
    CHECK(reusable);
}

// Random mix of inserts and erases, mirrored in std::map.
void test_mixed_workload_against_std_map() {
    int orders[5] = {3, 4, 5, 8, 64};
    bool sizeOk = true, resultsOk = true, validOk = true, contentsOk = true, rangesOk = true;
    for (int oi = 0; oi < 5; ++oi) {
        BPlusTree t(orders[oi]);
        std::map<int, int> ref;
        std::mt19937 rng(3000 + orders[oi]);
        std::uniform_int_distribution<int> keyDist(0, 3000);    // small range: lots of hits and misses
        for (int step = 0; step < 30000; ++step) {
            int k = keyDist(rng);
            if (rng() % 100 < 55) {                             // 55% insert, 45% erase
                int v = static_cast<int>(rng() % 100000);
                bool wasNew = (ref.find(k) == ref.end());
                if (t.insert(k, v) != wasNew) resultsOk = false;
                ref[k] = v;
            } else {
                bool existed = (ref.erase(k) > 0);
                if (t.erase(k) != existed) resultsOk = false;
            }
            if (t.size() != static_cast<long long>(ref.size())) sizeOk = false;
            if ((step < 1500 || step % 100 == 0) && !t.validate()) validOk = false;
        }
        Pairs want(ref.begin(), ref.end());
        if (t.toVector() != want) contentsOk = false;
        for (int q = 0; q < 200; ++q) {
            int a = keyDist(rng), b = keyDist(rng);
            if (a > b) std::swap(a, b);
            Pairs got = t.range(a, b), exp;
            for (std::map<int, int>::const_iterator it = ref.lower_bound(a);
                 it != ref.end() && it->first <= b; ++it) exp.push_back(*it);
            if (got != exp) rangesOk = false;
        }
    }
    CHECK(sizeOk);
    CHECK(resultsOk);
    CHECK(validOk);
    CHECK(contentsOk);
    CHECK(rangesOk);
}

void test_height_shrinks_after_deletes() {
    BPlusTree t(64);
    for (int i = 0; i < 100000; ++i) t.insert(i, i);
    int tall = t.height();
    for (int i = 10; i < 100000; ++i) t.erase(i);        // keep only keys 0..9
    CHECK(tall >= 3);
    CHECK(t.size() == 10);
    CHECK(t.height() == 1);                               // collapsed back to a single leaf
    CHECK(t.validate());
    int v = 0;
    CHECK(t.find(9, v) && v == 9);
    CHECK(!t.find(10, v));
}

// ---------- benchmark ----------

static long long usSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

void benchmark() {
    std::cout << "\nBenchmark: SKU lookup (node capacity 64). Build time, then per-lookup cost.\n";
    int sizes[3] = {10000, 100000, 1000000};
    for (int s = 0; s < 3; ++s) {
        const int n = sizes[s];
        std::vector<int> keys(n);
        for (int i = 0; i < n; ++i) keys[i] = i;
        std::mt19937 rng(77);
        std::shuffle(keys.begin(), keys.end(), rng);

        BPlusTree tree(64);
        std::map<int, int> rb;
        std::vector<std::pair<int, int> > flat;                      // unsorted list = linear scan
        flat.reserve(n);

        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) tree.insert(keys[i], keys[i] % 1000);
        long long buildUs = usSince(t0);
        for (int i = 0; i < n; ++i) { rb[keys[i]] = keys[i] % 1000; flat.push_back(std::make_pair(keys[i], keys[i] % 1000)); }

        const int Q = 100000;                                        // lookups for the fast structures
        std::vector<int> queries(Q);
        for (int i = 0; i < Q; ++i) queries[i] = static_cast<int>(rng() % n);

        long long checksum = 0;
        tree.resetComparisons();
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < Q; ++i) { int v = 0; tree.find(queries[i], v); checksum += v; }
        double treeUs = double(usSince(t0)) / Q;
        double treeCmp = double(tree.comparisons()) / Q;

        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < Q; ++i) checksum += rb.find(queries[i])->second;
        double rbUs = double(usSince(t0)) / Q;

        const int QL = 200;                                          // linear scan is slow: fewer queries
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < QL; ++i) {
            for (std::size_t j = 0; j < flat.size(); ++j)
                if (flat[j].first == queries[i]) { checksum += flat[j].second; break; }
        }
        double linUs = double(usSince(t0)) / QL;

        std::cout << "  n = " << n << "  height " << tree.height()
                  << "  build " << buildUs / 1000 << " ms\n"
                  << "     B+ Tree: " << treeUs << " us/lookup (" << treeCmp << " comparisons)"
                  << "   std::map: " << rbUs << " us/lookup"
                  << "   linear scan: " << linUs << " us/lookup"
                  << "   (checksum " << checksum % 1000 << ")\n";
    }

    std::cout << "\nBenchmark: range query vs scanning everything (n = 1,000,000, 1,000 results)\n";
    {
        const int n = 1000000;
        BPlusTree tree(64);
        std::vector<std::pair<int, int> > flat;
        for (int i = 0; i < n; ++i) { tree.insert(i, i); flat.push_back(std::make_pair(i, i)); }
        long long total = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (int q = 0; q < 1000; ++q) total += static_cast<long long>(tree.range(q * 900, q * 900 + 999).size());
        double treeUs = double(usSince(t0)) / 1000;
        t0 = std::chrono::steady_clock::now();
        for (int q = 0; q < 20; ++q) {
            int lo = q * 900, hi = q * 900 + 999;
            Pairs out;
            for (std::size_t j = 0; j < flat.size(); ++j)
                if (flat[j].first >= lo && flat[j].first <= hi) out.push_back(flat[j]);
            total += static_cast<long long>(out.size());
        }
        double scanUs = double(usSince(t0)) / 20;
        std::cout << "  B+ Tree range: " << treeUs << " us/query   full scan: " << scanUs
                  << " us/query   (results counted: " << total << ")\n";
    }

    std::cout << "\nBenchmark: delete (n = 1,000,000, node capacity 64)\n";
    {
        const int n = 1000000;
        std::vector<int> keys(n);
        for (int i = 0; i < n; ++i) keys[i] = i;
        std::mt19937 rng(5);
        std::shuffle(keys.begin(), keys.end(), rng);
        BPlusTree tree(64);
        for (int i = 0; i < n; ++i) tree.insert(keys[i], i);
        int before = tree.height();
        std::shuffle(keys.begin(), keys.end(), rng);
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n / 2; ++i) tree.erase(keys[i]);
        long long us = usSince(t0);
        std::cout << "  erased " << n / 2 << " random keys in " << us / 1000 << " ms ("
                  << double(us) / (n / 2) << " us each); height " << before << " -> " << tree.height()
                  << ", " << tree.size() << " left, structure valid: " << (tree.validate() ? "yes" : "NO") << "\n";
    }
}

int main() {
    test_empty_tree();
    test_small_inserts_and_find();
    test_update_existing_key();
    test_negative_and_extreme_keys();
    test_range_basics();
    test_orders_ascending_descending_random();
    test_against_std_map();
    test_height_stays_small();
    test_bad_order();
    test_erase_basics();
    test_erase_everything();
    test_mixed_workload_against_std_map();
    test_height_shrinks_after_deletes();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
