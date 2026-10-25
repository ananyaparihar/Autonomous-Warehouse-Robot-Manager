// mst.hpp - Minimum Spanning Tree two ways: Kruskal and Prim.
//
// Warehouse meaning: connect every station, rack zone and charging point with
// the least total conveyor belt / cable / rail length, so everything is linked
// but nothing is built twice. (Dijkstra answers "shortest route between two
// points"; the MST answers "cheapest network that touches all points".)
//
// A spanning tree of n nodes uses exactly n - 1 edges and has no cycles.
// The minimum one has the smallest total weight.
//
// Kruskal: sort all edges by weight; take an edge if it joins two different
//          groups (checked with union-find); skip it if it would form a cycle.
//          Time O(E log E), space O(V + E).
// Prim:    grow one tree from a start node; always add the cheapest edge that
//          reaches a new node (priority queue).
//          Time O(E log V) with a binary heap, space O(V + E).
//
// If the graph is NOT connected, both return a minimum spanning FOREST (one
// tree per connected piece) and set spanning = false.
// Negative weights are allowed. Self-loops and duplicate edges are harmless.
//
// Written to compile with older compilers (C++11).

#pragma once
#include <algorithm>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

struct MstEdge {
    int u;
    int v;
    long long w;
};

struct MstResult {
    bool spanning = false;          // true if one tree connects every node
    int components = 0;             // number of connected pieces
    long long totalWeight = 0;
    std::vector<MstEdge> edges;     // the chosen edges
};

// Union-find (disjoint sets): tracks which nodes are already connected.
// find() uses path halving, unite() joins the smaller group into the larger.
// Each operation is O(alpha(n)), which is effectively constant.
class DisjointSets {
public:
    explicit DisjointSets(int n) : parent_(n), size_(n, 1), groups_(n) {
        for (int i = 0; i < n; ++i) parent_[i] = i;
    }

    int find(int x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];   // path halving
            x = parent_[x];
        }
        return x;
    }

    // Returns true if a and b were in different groups (and are now joined).
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (size_[a] < size_[b]) std::swap(a, b);
        parent_[b] = a;
        size_[a] += size_[b];
        --groups_;
        return true;
    }

    int groups() const { return groups_; }

private:
    std::vector<int> parent_;
    std::vector<int> size_;
    int groups_;
};

class EdgeGraph {
public:
    explicit EdgeGraph(int n) : n_(n) {
        if (n < 1) throw std::invalid_argument("graph needs at least one node");
    }

    int size() const { return n_; }
    const std::vector<MstEdge>& edges() const { return edges_; }

    // Undirected edge u - v with weight w.
    void addEdge(int u, int v, long long w) {
        if (u < 0 || u >= n_ || v < 0 || v >= n_) throw std::out_of_range("node id out of range");
        MstEdge e = {u, v, w};
        edges_.push_back(e);
    }

    MstResult kruskal() const {
        std::vector<MstEdge> sorted = edges_;
        std::sort(sorted.begin(), sorted.end(), lighter);

        DisjointSets sets(n_);
        MstResult result;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (sets.unite(sorted[i].u, sorted[i].v)) {     // joins two groups: no cycle
                result.edges.push_back(sorted[i]);
                result.totalWeight += sorted[i].w;
                if (static_cast<int>(result.edges.size()) == n_ - 1) break;   // tree complete
            }
        }
        result.components = sets.groups();
        result.spanning = (result.components == 1);
        return result;
    }

    MstResult prim() const {
        std::vector<std::vector<std::pair<int, long long> > > adj(n_);
        for (std::size_t i = 0; i < edges_.size(); ++i) {
            adj[edges_[i].u].push_back(std::make_pair(edges_[i].v, edges_[i].w));
            adj[edges_[i].v].push_back(std::make_pair(edges_[i].u, edges_[i].w));
        }

        std::vector<bool> inTree(n_, false);
        MstResult result;

        // Restart from every node not yet reached, so a disconnected graph
        // gives a forest instead of silently dropping pieces.
        for (int start = 0; start < n_; ++start) {
            if (inTree[start]) continue;
            ++result.components;
            inTree[start] = true;

            std::priority_queue<Candidate, std::vector<Candidate>, Heavier> pq;
            pushEdgesFrom(start, adj, inTree, pq);

            while (!pq.empty()) {
                Candidate c = pq.top();
                pq.pop();
                if (inTree[c.to]) continue;                 // stale: that node is already in the tree
                inTree[c.to] = true;
                MstEdge e = {c.from, c.to, c.w};
                result.edges.push_back(e);
                result.totalWeight += c.w;
                pushEdgesFrom(c.to, adj, inTree, pq);
            }
        }
        result.spanning = (result.components == 1);
        return result;
    }

private:
    int n_;
    std::vector<MstEdge> edges_;

    static bool lighter(const MstEdge& a, const MstEdge& b) {
        if (a.w != b.w) return a.w < b.w;
        if (a.u != b.u) return a.u < b.u;       // ties broken the same way every run
        return a.v < b.v;
    }

    // An edge that could extend the tree: from a node inside it to a node outside.
    struct Candidate {
        long long w;
        int from;
        int to;
    };

    // Makes the priority queue return the LIGHTEST candidate first.
    struct Heavier {
        bool operator()(const Candidate& a, const Candidate& b) const { return a.w > b.w; }
    };

    typedef std::priority_queue<Candidate, std::vector<Candidate>, Heavier> CandidateQueue;

    static void pushEdgesFrom(int node,
                              const std::vector<std::vector<std::pair<int, long long> > >& adj,
                              const std::vector<bool>& inTree, CandidateQueue& pq) {
        for (std::size_t i = 0; i < adj[node].size(); ++i) {
            if (!inTree[adj[node][i].first]) {
                Candidate c = {adj[node][i].second, node, adj[node][i].first};
                pq.push(c);
            }
        }
    }
};
