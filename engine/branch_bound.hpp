// branch_bound.hpp - optimal multi-stop pick route by Branch and Bound.
//
// Warehouse meaning: a robot starts at the depot (node 0), must visit every
// pick stop (nodes 1..n-1) exactly once, and return to the depot. Find the
// ORDER of stops with the smallest total distance. (This is the Travelling
// Salesman Problem, which is NP-hard.)
//
// Idea:
//   BRANCH: build the route one stop at a time (depth-first search).
//   BOUND:  at each partial route, compute a LOWER BOUND on the cost of any
//           way to finish it. If that bound is already >= the best complete
//           route found so far, no completion can win, so cut the whole
//           subtree (prune it).
//
// Lower bound used: every node that still has to "leave" (the current node and
// each unvisited stop) must use at least its cheapest outgoing edge to a
// node that is still allowed (an unvisited stop or the depot). So
//     bound = cost so far + sum of those cheapest edges.
// Valid for symmetric and asymmetric distance matrices.
//
// Starting upper bound: a nearest-neighbour route (fast, usually decent),
// so pruning can begin immediately.
//
// Complexity: worst case O(n!) (pruning does not change the worst case),
// but in practice far fewer nodes are explored. Space: O(n) for the search,
// O(n^2) for the matrix.
//
// Written to compile with older compilers (C++11).

#pragma once
#include <algorithm>
#include <climits>
#include <stdexcept>
#include <utility>
#include <vector>

typedef std::vector<std::vector<long long> > DistMatrix;

// Use this value in the matrix for "no path between these two nodes".
const long long ROUTE_INF = LLONG_MAX / 4;

struct RouteResult {
    bool found = false;           // false if no route visits every stop
    long long cost = -1;          // total distance of the best route
    std::vector<int> route;       // e.g. 0, 3, 1, 2, 0
    long long nodesExplored = 0;  // search-tree nodes visited
    long long nodesPruned = 0;    // subtrees cut by the bound
};

class RoutePlanner {
public:
    // d[i][j] = distance from i to j. Node 0 is the depot.
    explicit RoutePlanner(const DistMatrix& d) : d_(d), n_(static_cast<int>(d.size())) {
        if (n_ == 0) throw std::invalid_argument("distance matrix is empty");
        for (int i = 0; i < n_; ++i) {
            if (static_cast<int>(d_[i].size()) != n_)
                throw std::invalid_argument("distance matrix must be square");
            for (int j = 0; j < n_; ++j)
                if (d_[i][j] < 0) throw std::invalid_argument("negative distance");
        }
    }

    // usePruning = true  -> Branch and Bound
    // usePruning = false -> plain brute force over all orders (for comparison)
    RouteResult solve(bool usePruning = true) {
        prune_ = usePruning;
        best_ = ROUTE_INF;
        bestRoute_.clear();
        explored_ = 0;
        pruned_ = 0;
        visited_.assign(n_, false);
        path_.assign(1, 0);
        visited_[0] = true;
        depth_ = 0;

        if (prune_) seedWithNearestNeighbour();

        dfs(0, 0);

        RouteResult result;
        result.nodesExplored = explored_;
        result.nodesPruned = pruned_;
        if (best_ < ROUTE_INF) {
            result.found = true;
            result.cost = best_;
            result.route = bestRoute_;
        }
        return result;
    }

private:
    DistMatrix d_;                        // own copy, so a temporary matrix is safe
    int n_;
    bool prune_ = true;
    long long best_ = ROUTE_INF;          // best complete route cost so far
    std::vector<int> bestRoute_;
    std::vector<bool> visited_;
    std::vector<int> path_;
    int depth_ = 0;                       // stops visited so far (excluding depot)
    long long explored_ = 0, pruned_ = 0;

    // Greedy route: always go to the nearest unvisited stop. Gives a first
    // upper bound. If it gets stuck (unreachable stop) we simply start with none.
    void seedWithNearestNeighbour() {
        std::vector<bool> seen(n_, false);
        seen[0] = true;
        std::vector<int> route(1, 0);
        long long cost = 0;
        int current = 0;
        for (int step = 1; step < n_; ++step) {
            int next = -1;
            long long bestD = ROUTE_INF;
            for (int v = 1; v < n_; ++v) {
                if (!seen[v] && d_[current][v] < bestD) { bestD = d_[current][v]; next = v; }
            }
            if (next == -1) return;               // stuck: no initial bound
            seen[next] = true;
            route.push_back(next);
            cost += bestD;
            current = next;
        }
        if (d_[current][0] >= ROUTE_INF) return;
        cost += d_[current][0];
        route.push_back(0);
        best_ = cost;
        bestRoute_ = route;
    }

    // Lower bound on the total cost of any complete route extending the
    // current partial route. Returns ROUTE_INF if no completion is possible.
    long long lowerBound(int current, long long cost) const {
        long long bound = cost;
        for (int u = 0; u < n_; ++u) {
            if (u != current && (u == 0 || visited_[u])) continue;   // only current + unvisited stops
            long long cheapest = ROUTE_INF;
            for (int v = 0; v < n_; ++v) {
                if (v == u) continue;
                if (v != 0 && visited_[v]) continue;                 // allowed targets: unvisited stops or depot
                if (d_[u][v] < cheapest) cheapest = d_[u][v];
            }
            if (cheapest >= ROUTE_INF) return ROUTE_INF;             // this node can never leave
            bound += cheapest;
        }
        return bound;
    }

    void dfs(int current, long long cost) {
        ++explored_;

        // All stops visited: close the loop back to the depot.
        if (depth_ == n_ - 1) {
            if (d_[current][0] < ROUTE_INF && cost + d_[current][0] < best_) {
                best_ = cost + d_[current][0];
                bestRoute_ = path_;
                bestRoute_.push_back(0);
            }
            return;
        }

        // BOUND: can this partial route still beat the best one?
        if (prune_ && lowerBound(current, cost) >= best_) {
            ++pruned_;
            return;
        }

        // BRANCH: try each unvisited stop, nearest first (finds good routes sooner).
        std::vector<std::pair<long long, int> > next;
        for (int v = 1; v < n_; ++v) {
            if (!visited_[v] && d_[current][v] < ROUTE_INF)
                next.push_back(std::make_pair(d_[current][v], v));
        }
        std::sort(next.begin(), next.end());

        for (std::size_t k = 0; k < next.size(); ++k) {
            int v = next[k].second;
            visited_[v] = true;
            path_.push_back(v);
            ++depth_;
            dfs(v, cost + next[k].first);
            --depth_;
            path_.pop_back();
            visited_[v] = false;
        }
    }
};
