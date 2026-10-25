// dijkstra.hpp - weighted graph + Dijkstra's shortest path.
//
// Complexity: O((V + E) log V) with a binary-heap priority queue.
// Space: O(V + E) for the adjacency list, O(V) for distances.
// Requires non-negative edge weights.

#pragma once
#include <algorithm>
#include <climits>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

struct PathResult {
    bool reachable = false;     // false if no path exists
    long long distance = -1;    // total cost, -1 if unreachable
    std::vector<int> path;      // node ids from source to target (empty if unreachable)
};

class Graph {
public:
    explicit Graph(int n) : adj_(n) {}

    int size() const { return static_cast<int>(adj_.size()); }

    // Adds an undirected edge u <-> v with weight w (w >= 0).
    void addEdge(int u, int v, int w) {
        check(u); check(v);
        if (w < 0) throw std::invalid_argument("negative weight not allowed");
        adj_[u].push_back({v, w});
        adj_[v].push_back({u, w});
    }

    // Shortest path from source to target.
    PathResult shortestPath(int source, int target) const {
        check(source); check(target);
        const int n = size();
        const long long INF = LLONG_MAX / 4;

        std::vector<long long> dist(n, INF);
        std::vector<int> parent(n, -1);   // parent[v] = node we came from to reach v

        // Min-heap of (distance, node). greater<> makes the smallest distance come out first.
        using State = std::pair<long long, int>;
        std::priority_queue<State, std::vector<State>, std::greater<State>> pq;

        dist[source] = 0;
        pq.push({0, source});

        while (!pq.empty()) {
            long long d = pq.top().first;    // distance when this entry was pushed
            int u = pq.top().second;         // the node
            pq.pop();

            // A node can be pushed several times. If this entry is older (longer)
            // than the best distance we already know, skip it.
            if (d > dist[u]) continue;

            if (u == target) break;   // shortest distance to target is final

            for (const auto& edge : adj_[u]) {
                int v = edge.first;    // neighbour
                int w = edge.second;   // edge weight
                // Relaxation: is going through u a shorter way to v?
                if (dist[u] + w < dist[v]) {
                    dist[v] = dist[u] + w;
                    parent[v] = u;
                    pq.push({dist[v], v});
                }
            }
        }

        PathResult result;
        if (dist[target] == INF) return result;   // unreachable

        result.reachable = true;
        result.distance = dist[target];
        // Rebuild the path by walking parents back from target, then reverse.
        for (int v = target; v != -1; v = parent[v]) result.path.push_back(v);
        std::reverse(result.path.begin(), result.path.end());
        return result;
    }

private:
    std::vector<std::vector<std::pair<int, int>>> adj_;   // adj_[u] = list of (neighbour, weight)

    void check(int v) const {
        if (v < 0 || v >= size()) throw std::out_of_range("node id out of range");
    }
};
