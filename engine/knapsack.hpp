// knapsack.hpp - 0/1 knapsack by dynamic programming (+ a greedy baseline).
//
// Warehouse meaning: a robot can carry at most `capacity` kg. Each pending
// order has a weight and a value (priority / revenue). Pick the set of orders
// with the highest total value that fits in one trip. Each order is taken
// whole or not at all (that is the "0/1" part).
//
// DP complexity:     time O(n * W), space O(n * W)   (n = items, W = capacity)
// Greedy complexity: time O(n log n), space O(n)     (fast, but NOT always optimal)
//
// Written to compile with older compilers (C++11).

#pragma once
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <vector>

struct Item {
    int id;       // order id
    int weight;   // payload used
    int value;    // benefit of carrying it
};

struct KnapsackResult {
    int totalValue = 0;
    int totalWeight = 0;
    std::vector<int> chosen;   // ids of the chosen items
};

namespace knapsack_detail {
inline void validate(const std::vector<Item>& items, int capacity) {
    if (capacity < 0) throw std::invalid_argument("capacity must be >= 0");
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].weight < 0 || items[i].value < 0)
            throw std::invalid_argument("weight and value must be >= 0");
    }
}
}  // namespace knapsack_detail

// Optimal answer.
//
// dp[i][c] = best total value using only the first i items with capacity c.
//   Skip item i:  dp[i-1][c]
//   Take item i:  dp[i-1][c - weight] + value     (only if weight <= c)
//   dp[i][c] = the larger of the two.
inline KnapsackResult knapsackDP(const std::vector<Item>& items, int capacity) {
    knapsack_detail::validate(items, capacity);
    const int n = static_cast<int>(items.size());

    std::vector<std::vector<int> > dp(n + 1, std::vector<int>(capacity + 1, 0));

    for (int i = 1; i <= n; ++i) {
        const int w = items[i - 1].weight;
        const int v = items[i - 1].value;
        for (int c = 0; c <= capacity; ++c) {
            dp[i][c] = dp[i - 1][c];                       // skip
            if (w <= c) {
                int take = dp[i - 1][c - w] + v;           // take
                if (take > dp[i][c]) dp[i][c] = take;
            }
        }
    }

    // Walk the table backwards to find WHICH items were taken.
    KnapsackResult result;
    result.totalValue = dp[n][capacity];
    int c = capacity;
    for (int i = n; i >= 1; --i) {
        if (dp[i][c] != dp[i - 1][c]) {     // value changed, so item i was taken
            result.chosen.push_back(items[i - 1].id);
            result.totalWeight += items[i - 1].weight;
            c -= items[i - 1].weight;
        }
    }
    std::reverse(result.chosen.begin(), result.chosen.end());
    return result;
}

// Greedy baseline: take items by best value-per-kg first while they fit.
// Included to compare against the DP in the report.
inline KnapsackResult knapsackGreedy(const std::vector<Item>& items, int capacity) {
    knapsack_detail::validate(items, capacity);

    std::vector<int> order(items.size());
    std::iota(order.begin(), order.end(), 0);
    // a before b if value_a / weight_a > value_b / weight_b (cross-multiplied, no division)
    std::sort(order.begin(), order.end(), [&items](int a, int b) {
        long long left = static_cast<long long>(items[a].value) * items[b].weight;
        long long right = static_cast<long long>(items[b].value) * items[a].weight;
        return left > right;
    });

    KnapsackResult result;
    int used = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Item& it = items[order[k]];
        if (used + it.weight <= capacity) {
            used += it.weight;
            result.totalValue += it.value;
            result.totalWeight += it.weight;
            result.chosen.push_back(it.id);
        }
    }
    std::sort(result.chosen.begin(), result.chosen.end());
    return result;
}
