// trip_planner.hpp - plans ONE robot trip by chaining the engine's modules.
//
//   orders --> [1 B+ Tree]   look up each order's SKU -> shelf location (floor node)
//          --> [2 Union-Find] drop orders whose shelf the robot cannot reach
//          --> [3 Min-Heap]  take the most URGENT orders as the candidate pool
//          --> [4 Knapsack]  choose the most valuable mix that fits the payload
//          --> [5 Dijkstra]  real floor distance between every pair of stops
//          --> [6 Branch & Bound] best ORDER to visit the stops (depot -> ... -> depot)
//          --> [5 Dijkstra]  expand that order into the full list of floor nodes
//
// The planner works on plain C++ structs. The command-line program
// (engine_cli.cpp) only converts JSON <-> these structs, so this file can be
// tested directly and has no knowledge of JSON.
//
// Written to compile with older compilers (C++11).

#pragma once
#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "bplus_tree.hpp"
#include "branch_bound.hpp"
#include "dijkstra.hpp"
#include "knapsack.hpp"
#include "min_heap.hpp"
#include "mst.hpp"          // for DisjointSets (reachability check)

// Limits keep one request from using unbounded time or memory.
const int PLAN_MAX_NODES = 2000000;
const int PLAN_MAX_CAPACITY = 1000000;     // knapsack table is (candidates x capacity)
const int PLAN_MAX_CANDIDATES = 14;        // Branch and Bound is factorial in the worst case
const int PLAN_DEFAULT_CANDIDATES = 12;

struct FloorEdge {
    int u;
    int v;
    int w;
};

struct SkuLocation {
    int sku;
    int node;              // floor node where this SKU's bin is
};

struct PlanOrder {
    long long id;          // the caller's order id (only used as a label)
    int sku;
    int weight;            // payload used on the robot
    int value;             // benefit of carrying it
    int urgency;           // lower = more urgent
};

struct TripRequest {
    int nodes = 0;
    std::vector<FloorEdge> edges;
    int depot = 0;
    int capacity = 0;
    int maxCandidates = PLAN_DEFAULT_CANDIDATES;
    std::vector<SkuLocation> inventory;
    std::vector<PlanOrder> orders;
};

struct RejectedOrder {
    long long id;
    std::string reason;    // "unknown_sku" or "unreachable_from_depot"
};

struct TripStop {
    int node;
    std::vector<long long> orderIds;   // orders picked up at this node
};

struct TripPlan {
    std::vector<long long> selected;       // order ids on this trip (most urgent first)
    std::vector<long long> deferred;       // valid orders NOT on this trip, in urgency order
    std::vector<RejectedOrder> rejected;   // orders that can never be served as given
    std::vector<TripStop> stops;           // visit order (depot stop first, if any orders sit at the depot)
    std::vector<int> route;                // full floor path: depot, ..., depot
    long long distance = 0;                // total length of `route`
    long long totalValue = 0;
    long long totalWeight = 0;
    // statistics, for the report
    int candidates = 0;                    // orders the heap handed to the knapsack
    long long boundNodesExplored = 0;
    long long boundNodesPruned = 0;
};

inline TripPlan planTrip(const TripRequest& req) {
    // ---------- validate ----------
    if (req.nodes < 1 || req.nodes > PLAN_MAX_NODES)
        throw std::invalid_argument("floor.nodes must be between 1 and 2000000");
    if (req.depot < 0 || req.depot >= req.nodes)
        throw std::invalid_argument("depot is not a node of the floor");
    if (req.capacity < 0 || req.capacity > PLAN_MAX_CAPACITY)
        throw std::invalid_argument("capacity must be between 0 and 1000000");
    if (req.maxCandidates < 1 || req.maxCandidates > PLAN_MAX_CANDIDATES)
        throw std::invalid_argument("max_candidates must be between 1 and 14");
    for (std::size_t i = 0; i < req.edges.size(); ++i) {
        const FloorEdge& e = req.edges[i];
        if (e.u < 0 || e.u >= req.nodes || e.v < 0 || e.v >= req.nodes)
            throw std::invalid_argument("an edge refers to a node outside the floor");
        if (e.w < 0) throw std::invalid_argument("edge lengths cannot be negative");
    }
    for (std::size_t i = 0; i < req.inventory.size(); ++i) {
        if (req.inventory[i].node < 0 || req.inventory[i].node >= req.nodes)
            throw std::invalid_argument("an inventory entry points to a node outside the floor");
    }
    for (std::size_t i = 0; i < req.orders.size(); ++i) {
        if (req.orders[i].weight < 0 || req.orders[i].value < 0)
            throw std::invalid_argument("order weight and value cannot be negative");
    }

    TripPlan plan;

    // ---------- build the floor and a connectivity map ----------
    Graph floor(req.nodes);
    DisjointSets connected(req.nodes);
    for (std::size_t i = 0; i < req.edges.size(); ++i) {
        floor.addEdge(req.edges[i].u, req.edges[i].v, req.edges[i].w);
        connected.unite(req.edges[i].u, req.edges[i].v);
    }

    // ---------- [1] B+ Tree: SKU -> shelf location ----------
    BPlusTree index(64);
    for (std::size_t i = 0; i < req.inventory.size(); ++i)
        index.insert(req.inventory[i].sku, req.inventory[i].node);

    // ---------- [2] + [3] filter, then Min-Heap by urgency ----------
    std::vector<int> orderNode(req.orders.size(), -1);
    MinHeap heap;
    for (std::size_t i = 0; i < req.orders.size(); ++i) {
        int node = 0;
        if (!index.find(req.orders[i].sku, node)) {
            RejectedOrder r;
            r.id = req.orders[i].id;
            r.reason = "unknown_sku";
            plan.rejected.push_back(r);
            continue;
        }
        if (connected.find(node) != connected.find(req.depot)) {
            RejectedOrder r;
            r.id = req.orders[i].id;
            r.reason = "unreachable_from_depot";
            plan.rejected.push_back(r);
            continue;
        }
        orderNode[i] = node;
        Task t;
        t.id = static_cast<int>(i);              // position in req.orders
        t.urgency = req.orders[i].urgency;
        heap.push(t);
    }

    // The heap hands over the most urgent orders: the candidate pool.
    std::vector<int> candidates;
    while (!heap.empty() && static_cast<int>(candidates.size()) < req.maxCandidates)
        candidates.push_back(heap.pop().id);
    std::vector<int> notConsidered;              // still queued: not urgent enough this round
    while (!heap.empty()) notConsidered.push_back(heap.pop().id);
    plan.candidates = static_cast<int>(candidates.size());

    // ---------- [4] Knapsack: best mix within the payload ----------
    std::vector<Item> items;
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        Item it;
        it.id = static_cast<int>(c);             // position in `candidates`
        it.weight = req.orders[candidates[c]].weight;
        it.value = req.orders[candidates[c]].value;
        items.push_back(it);
    }
    KnapsackResult pick = knapsackDP(items, req.capacity);
    plan.totalValue = pick.totalValue;
    plan.totalWeight = pick.totalWeight;

    std::vector<bool> chosen(candidates.size(), false);
    for (std::size_t k = 0; k < pick.chosen.size(); ++k) chosen[pick.chosen[k]] = true;

    std::vector<int> selectedIdx;                // indexes into req.orders, most urgent first
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        if (chosen[c]) {
            selectedIdx.push_back(candidates[c]);
            plan.selected.push_back(req.orders[candidates[c]].id);
        } else {
            plan.deferred.push_back(req.orders[candidates[c]].id);
        }
    }
    for (std::size_t k = 0; k < notConsidered.size(); ++k)
        plan.deferred.push_back(req.orders[notConsidered[k]].id);

    // ---------- group the chosen orders by shelf location ----------
    std::vector<long long> depotOrders;          // orders whose shelf IS the depot: zero travel
    std::vector<int> stopNodes;                  // distinct non-depot shelf nodes
    std::vector<std::vector<long long> > ordersAtStop;
    std::map<int, int> stopOfNode;
    for (std::size_t s = 0; s < selectedIdx.size(); ++s) {
        int node = orderNode[selectedIdx[s]];
        long long id = req.orders[selectedIdx[s]].id;
        if (node == req.depot) {
            depotOrders.push_back(id);
            continue;
        }
        std::map<int, int>::iterator it = stopOfNode.find(node);
        if (it == stopOfNode.end()) {
            stopOfNode[node] = static_cast<int>(stopNodes.size());
            stopNodes.push_back(node);
            ordersAtStop.push_back(std::vector<long long>());
            ordersAtStop.back().push_back(id);
        } else {
            ordersAtStop[it->second].push_back(id);
        }
    }

    plan.route.push_back(req.depot);
    if (!depotOrders.empty()) {
        TripStop s;
        s.node = req.depot;
        s.orderIds = depotOrders;
        plan.stops.push_back(s);
    }
    if (stopNodes.empty()) return plan;          // nothing to travel to: the trip is just the depot

    // ---------- [5] Dijkstra: floor distances between all pairs of stops ----------
    const int m = static_cast<int>(stopNodes.size()) + 1;      // matrix index 0 = depot
    std::vector<int> nodeOf(m);
    nodeOf[0] = req.depot;
    for (int i = 1; i < m; ++i) nodeOf[i] = stopNodes[i - 1];

    DistMatrix dist(m, std::vector<long long>(m, 0));
    std::vector<std::vector<std::vector<int> > > pathBetween(m, std::vector<std::vector<int> >(m));
    for (int i = 0; i < m; ++i) {
        for (int j = i + 1; j < m; ++j) {
            PathResult p = floor.shortestPath(nodeOf[i], nodeOf[j]);
            long long d = p.reachable ? p.distance : ROUTE_INF;
            dist[i][j] = d;
            dist[j][i] = d;
            pathBetween[i][j] = p.path;
            pathBetween[j][i] = std::vector<int>(p.path.rbegin(), p.path.rend());
        }
    }

    // ---------- [6] Branch and Bound: best order of the stops ----------
    RoutePlanner planner(dist);
    RouteResult best = planner.solve(true);
    if (!best.found) throw std::runtime_error("no route visits every selected stop");
    plan.distance = best.cost;
    plan.boundNodesExplored = best.nodesExplored;
    plan.boundNodesPruned = best.nodesPruned;

    // ---------- expand the stop order into real floor nodes ----------
    for (std::size_t t = 0; t + 1 < best.route.size(); ++t) {
        const std::vector<int>& leg = pathBetween[best.route[t]][best.route[t + 1]];
        for (std::size_t k = 1; k < leg.size(); ++k) plan.route.push_back(leg[k]);   // skip leg[0]: already there
    }
    for (std::size_t t = 1; t + 1 < best.route.size(); ++t) {
        TripStop s;
        s.node = nodeOf[best.route[t]];
        s.orderIds = ordersAtStop[best.route[t] - 1];
        plan.stops.push_back(s);
    }
    return plan;
}

// ---------- fleet: several robots share the same waiting orders ----------
struct FleetPlan {
    std::vector<TripPlan> trips;   // trips[0] = robot 1, trips[1] = robot 2, ...
};

inline FleetPlan planFleet(const TripRequest& req, int robots) {
    if (robots < 1 || robots > 20)
        throw std::invalid_argument("robots must be between 1 and 20");

    FleetPlan fleet;
    TripRequest left = req;                      // orders still waiting

    for (int r = 0; r < robots; ++r) {
        TripPlan trip = planTrip(left);          // this robot takes the most urgent orders it can carry
        fleet.trips.push_back(trip);
        if (trip.selected.empty()) break;        // nothing left for the next robot

        // remove the orders this robot took
        std::vector<PlanOrder> remaining;
        for (std::size_t i = 0; i < left.orders.size(); ++i) {
            bool taken = false;
            for (std::size_t k = 0; k < trip.selected.size(); ++k)
                if (left.orders[i].id == trip.selected[k]) taken = true;
            if (!taken) remaining.push_back(left.orders[i]);
        }
        left.orders = remaining;
    }
    return fleet;
}
