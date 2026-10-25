// test_avl_tree.cpp - correctness tests + benchmark for the AVL tree.
// Build:  g++ -std=c++11 -O2 test_avl_tree.cpp -o test_avl_tree
// Run:    .\test_avl_tree.exe

#include "avl_tree.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
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
typedef std::vector<int> Ints;

// An AVL tree with n nodes can never be taller than about 1.4405 * log2(n + 2).
static int avlHeightBound(long long n) {
    return static_cast<int>(std::ceil(1.4405 * std::log(double(n) + 2.0) / std::log(2.0)));
}

// A plain, unbalanced binary search tree. Only here to show WHY balancing matters.
struct NaiveBst {
    struct N { int key; int l; int r; };
    std::vector<N> nodes;
    long long comparisons;
    NaiveBst() : comparisons(0) {}

    void insert(int key) {
        if (nodes.empty()) { N first = {key, -1, -1}; nodes.push_back(first); return; }
        int cur = 0;
        for (;;) {
            if (key == nodes[cur].key) return;
            bool goLeft = key < nodes[cur].key;
            int next = goLeft ? nodes[cur].l : nodes[cur].r;
            if (next < 0) {
                N fresh = {key, -1, -1};
                nodes.push_back(fresh);
                int id = static_cast<int>(nodes.size()) - 1;
                if (goLeft) nodes[cur].l = id; else nodes[cur].r = id;
                return;
            }
            cur = next;
        }
    }

    bool find(int key) {
        int cur = nodes.empty() ? -1 : 0;
        while (cur >= 0) {
            ++comparisons;
            if (key == nodes[cur].key) return true;
            cur = key < nodes[cur].key ? nodes[cur].l : nodes[cur].r;
        }
        return false;
    }

    int height() const {                         // level by level, no recursion (it could be 100,000 deep)
        if (nodes.empty()) return 0;
        std::vector<int> level(1, 0);
        int h = 0;
        while (!level.empty()) {
            ++h;
            std::vector<int> next;
            for (std::size_t i = 0; i < level.size(); ++i) {
                if (nodes[level[i]].l >= 0) next.push_back(nodes[level[i]].l);
                if (nodes[level[i]].r >= 0) next.push_back(nodes[level[i]].r);
            }
            level.swap(next);
        }
        return h;
    }
};

void test_empty_tree() {
    AvlTree t;
    int v = 0;
    CHECK(t.empty() && t.size() == 0 && t.height() == 0);
    CHECK(!t.find(1, v));
    CHECK(!t.erase(1));
    CHECK(t.inorder().empty() && t.preorder().empty() && t.postorder().empty());
    CHECK(t.levelOrder().empty());
    CHECK(t.range(0, 10).empty());
    CHECK(t.validate());
}

// The four rotation cases. Each three-key insert order forces a different one.
// In every case the middle key (20) must end up as the root.
void test_four_rotation_cases() {
    int orders[4][3] = {{30, 20, 10},    // Left-Left    -> 1 rotation
                        {10, 20, 30},    // Right-Right  -> 1 rotation
                        {30, 10, 20},    // Left-Right   -> 2 rotations
                        {10, 30, 20}};   // Right-Left   -> 2 rotations
    long long expectedRotations[4] = {1, 1, 2, 2};
    for (int c = 0; c < 4; ++c) {
        AvlTree t;
        for (int i = 0; i < 3; ++i) t.insert(orders[c][i], 0);
        CHECK(t.height() == 2);
        CHECK((t.levelOrder()[0] == Ints(1, 20)));
        CHECK((t.inorder() == Ints{10, 20, 30}));
        CHECK(t.rotations() == expectedRotations[c]);
        CHECK(t.validate());
    }
}

void test_traversals_on_known_tree() {
    AvlTree t;
    int keys[7] = {50, 30, 70, 20, 40, 60, 80};          // inserts into a perfect tree, no rotations
    for (int i = 0; i < 7; ++i) t.insert(keys[i], keys[i] * 10);
    CHECK(t.rotations() == 0);
    CHECK((t.inorder() == Ints{20, 30, 40, 50, 60, 70, 80}));
    CHECK((t.preorder() == Ints{50, 30, 20, 40, 70, 60, 80}));
    CHECK((t.postorder() == Ints{20, 40, 30, 60, 80, 70, 50}));
    std::vector<Ints> levels = t.levelOrder();
    CHECK(levels.size() == 3);
    CHECK((levels[0] == Ints{50}));
    CHECK((levels[1] == Ints{30, 70}));
    CHECK((levels[2] == Ints{20, 40, 60, 80}));
    Pairs all = t.toVector();
    CHECK(all.size() == 7 && all[0].second == 200 && all[6].second == 800);
}

void test_find_and_update() {
    AvlTree t;
    CHECK(t.insert(5, 50));
    CHECK(t.insert(3, 30));
    CHECK(!t.insert(5, 99));                       // existing key: update, not a new node
    CHECK(t.size() == 2);
    int v = 0;
    CHECK(t.find(5, v) && v == 99);
    CHECK(t.find(3, v) && v == 30);
    CHECK(!t.find(4, v));
    CHECK(t.contains(3) && !t.contains(7));
    CHECK(t.validate());
}

void test_range() {
    AvlTree t;
    for (int k = 0; k < 100; k += 2) t.insert(k, k + 1000);   // even keys 0..98
    Pairs r = t.range(10, 20);
    CHECK(r.size() == 6 && r.front().first == 10 && r.back().first == 20);
    CHECK(t.range(11, 11).empty());
    CHECK(t.range(30, 20).empty());
    CHECK(t.range(-5, 1000).size() == 50);
    CHECK(t.range(98, 98).size() == 1);
    CHECK(t.range(200, 300).empty());
}

void test_sorted_insert_stays_balanced() {
    bool allBalanced = true, heightsOk = true;
    int counts[5] = {7, 100, 1000, 1023, 5000};
    for (int c = 0; c < 5; ++c) {
        AvlTree up, down;
        for (int i = 0; i < counts[c]; ++i) { up.insert(i, i); down.insert(counts[c] - i, i); }
        if (!up.validate() || !down.validate()) allBalanced = false;
        if (up.height() > avlHeightBound(counts[c]) || down.height() > avlHeightBound(counts[c])) heightsOk = false;
    }
    CHECK(allBalanced);
    CHECK(heightsOk);
}

void test_why_balancing_matters() {
    const int n = 2000;
    NaiveBst naive;
    AvlTree avl;
    for (int i = 0; i < n; ++i) { naive.insert(i); avl.insert(i, i); }   // keys arrive sorted
    CHECK(naive.height() == n);                    // the plain BST became a linked list
    CHECK(avl.height() <= 15);                     // the AVL tree is still short
    naive.find(n - 1);
    CHECK(naive.comparisons == n);                 // one lookup costs n comparisons
    avl.resetCounters();
    int v = 0;
    avl.find(n - 1, v);
    CHECK(avl.comparisons() <= 15);
}

void test_erase_cases() {
    AvlTree t;
    int keys[9] = {50, 30, 70, 20, 40, 60, 80, 10, 25};
    for (int i = 0; i < 9; ++i) t.insert(keys[i], keys[i]);
    CHECK(!t.erase(999));                          // missing key
    CHECK(t.size() == 9);
    CHECK(t.erase(10));                            // leaf
    CHECK(t.validate());
    CHECK(t.erase(20));                            // node with one child (25)
    CHECK(t.validate());
    CHECK(t.erase(30));                            // node with two children
    CHECK(t.validate());
    CHECK(t.erase(50));                            // the root
    CHECK(t.validate());
    CHECK(t.size() == 5);
    CHECK((t.inorder() == Ints{25, 40, 60, 70, 80}));
    int v = 0;
    CHECK(!t.find(50, v));
}

// Erase every key in several orders, validating after EVERY erase.
void test_erase_everything() {
    bool ok = true, endsEmpty = true;
    for (int pattern = 0; pattern < 3; ++pattern) {
        for (int n = 1; n <= 200; n += (n < 20 ? 1 : 37)) {
            Ints order;
            for (int i = 0; i < n; ++i) order.push_back(i);
            if (pattern == 1) std::reverse(order.begin(), order.end());
            if (pattern == 2) { std::mt19937 rng(n); std::shuffle(order.begin(), order.end(), rng); }
            AvlTree t;
            for (int i = 0; i < n; ++i) t.insert(i, i);
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (!t.erase(order[i])) ok = false;
                if (!t.validate()) ok = false;
            }
            if (!t.empty() || t.height() != 0) endsEmpty = false;
        }
    }
    CHECK(ok);
    CHECK(endsEmpty);
}

void test_mixed_workload_against_std_map() {
    bool sizeOk = true, resultsOk = true, validOk = true, contentsOk = true, rangesOk = true, heightOk = true;
    AvlTree t;
    std::map<int, int> ref;
    std::mt19937 rng(77);
    std::uniform_int_distribution<int> keyDist(0, 3000);
    for (int step = 0; step < 40000; ++step) {
        int k = keyDist(rng);
        if (rng() % 100 < 55) {
            int v = static_cast<int>(rng() % 100000);
            bool wasNew = (ref.find(k) == ref.end());
            if (t.insert(k, v) != wasNew) resultsOk = false;
            ref[k] = v;
        } else {
            bool existed = (ref.erase(k) > 0);
            if (t.erase(k) != existed) resultsOk = false;
        }
        if (t.size() != static_cast<long long>(ref.size())) sizeOk = false;
        if ((step < 2000 || step % 100 == 0) && !t.validate()) validOk = false;
        if (t.height() > avlHeightBound(t.size())) heightOk = false;
    }
    Pairs want(ref.begin(), ref.end());
    if (t.toVector() != want) contentsOk = false;
    for (int q = 0; q < 300; ++q) {
        int a = keyDist(rng), b = keyDist(rng);
        if (a > b) std::swap(a, b);
        Pairs got = t.range(a, b), exp;
        for (std::map<int, int>::const_iterator it = ref.lower_bound(a);
             it != ref.end() && it->first <= b; ++it) exp.push_back(*it);
        if (got != exp) rangesOk = false;
    }
    CHECK(sizeOk);
    CHECK(resultsOk);
    CHECK(validOk);
    CHECK(contentsOk);
    CHECK(rangesOk);
    CHECK(heightOk);
}

// ---------- benchmark ----------

static long long usSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

void benchmark() {
    std::cout << "\nBenchmark 1: plain BST vs AVL when keys arrive SORTED (lookup of the last key)\n";
    int smallSizes[3] = {1000, 10000, 30000};
    for (int s = 0; s < 3; ++s) {
        const int n = smallSizes[s];
        NaiveBst naive;
        AvlTree avl;
        for (int i = 0; i < n; ++i) { naive.insert(i); avl.insert(i, i); }
        naive.find(n - 1);
        avl.resetCounters();
        int v = 0;
        avl.find(n - 1, v);
        std::cout << "  n = " << n << "   plain BST: height " << naive.height() << ", "
                  << naive.comparisons << " comparisons"
                  << "   AVL: height " << avl.height() << ", " << avl.comparisons() << " comparisons\n";
    }

    std::cout << "\nBenchmark 2: AVL vs std::map (a Red-Black tree), random insert order\n";
    int sizes[3] = {10000, 100000, 1000000};
    for (int s = 0; s < 3; ++s) {
        const int n = sizes[s];
        std::vector<int> keys(n);
        for (int i = 0; i < n; ++i) keys[i] = i;
        std::mt19937 rng(31);
        std::shuffle(keys.begin(), keys.end(), rng);

        AvlTree avl;
        std::map<int, int> rb;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) avl.insert(keys[i], i);
        long long avlBuildMs = usSince(t0) / 1000;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) rb[keys[i]] = i;
        long long rbBuildMs = usSince(t0) / 1000;

        const double rotationsPerInsert = double(avl.rotations()) / n;   // read BEFORE resetting counters

        const int Q = 200000;
        std::vector<int> queries(Q);
        for (int i = 0; i < Q; ++i) queries[i] = static_cast<int>(rng() % n);
        long long checksum = 0;
        avl.resetCounters();
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < Q; ++i) { int v = 0; avl.find(queries[i], v); checksum += v; }
        double avlUs = double(usSince(t0)) / Q;
        double avlCmp = double(avl.comparisons()) / Q;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < Q; ++i) checksum += rb.find(queries[i])->second;
        double rbUs = double(usSince(t0)) / Q;

        std::cout << "  n = " << n << "  AVL height " << avl.height() << " (bound " << avlHeightBound(n) << ")"
                  << "  rotations/insert " << rotationsPerInsert << "\n"
                  << "     build: AVL " << avlBuildMs << " ms, std::map " << rbBuildMs << " ms"
                  << "   lookup: AVL " << avlUs << " us (" << avlCmp << " comparisons), std::map " << rbUs << " us"
                  << "   (checksum " << checksum % 1000 << ")\n";
    }

    std::cout << "\nBenchmark 3: AVL with sorted inserts (worst case for a plain BST), n = 1,000,000\n";
    {
        const int n = 1000000;
        AvlTree avl;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) avl.insert(i, i);
        long long ms = usSince(t0) / 1000;
        std::cout << "  built in " << ms << " ms, height " << avl.height() << ", rotations/insert "
                  << double(avl.rotations()) / n << ", valid: " << (avl.validate() ? "yes" : "NO") << "\n";
    }
}

int main() {
    test_empty_tree();
    test_four_rotation_cases();
    test_traversals_on_known_tree();
    test_find_and_update();
    test_range();
    test_sorted_insert_stays_balanced();
    test_why_balancing_matters();
    test_erase_cases();
    test_erase_everything();
    test_mixed_workload_against_std_map();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    if (failed == 0) benchmark();
    return failed == 0 ? 0 : 1;
}
