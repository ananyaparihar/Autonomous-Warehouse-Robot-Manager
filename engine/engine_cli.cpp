// engine_cli.cpp - the engine as a command-line program: JSON in, JSON out.
//
// Build:  g++ -std=c++14 -O2 engine_cli.cpp -o engine_cli
//
// Use:    engine_cli                      reads ONE JSON request from stdin
//         engine_cli --file request.json  reads the request from a file
//         engine_cli --pretty ...         indents the response so a human can read it
//         engine_cli --help
//
// Every response is a single JSON object with "ok": true or "ok": false.
// On failure it carries "error": "<what went wrong>" and the exit code is 1.
// (Exit code 2 means the program itself was started with bad arguments.)
//
// Operations ("op" field of the request):
//   ping           check that the engine runs
//   plan_trip      the full pipeline: orders -> one optimised robot trip
//   shortest_path  Dijkstra between two floor nodes
//   network_mst    cheapest network connecting every floor node (Kruskal or Prim)
//   lookup_sku     B+ Tree lookup of one SKU's shelf location
//   sku_range      B+ Tree range query over SKU ids
// See README.md for the exact request and response fields.

#include <chrono>
#include <climits>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "json.hpp"
#include "trip_planner.hpp"

static const char* ENGINE_VERSION = "1.0";

// ---------- small helpers for reading fields out of the request ----------

static int toInt(long long v, const std::string& what) {
    if (v < INT_MIN || v > INT_MAX) throw std::invalid_argument(what + " is out of range");
    return static_cast<int>(v);
}

static int getInt(const Json& obj, const char* key) {
    return toInt(obj.get(key).asInt(), key);
}

static int getIntOr(const Json& obj, const char* key, int fallback) {
    const Json* p = obj.find(key);
    if (p == nullptr || p->isNull()) return fallback;
    return toInt(p->asInt(), key);
}

static Json intArray(const std::vector<int>& v) {
    Json a = Json::makeArray();
    for (std::size_t i = 0; i < v.size(); ++i) a.push(Json::makeInt(v[i]));
    return a;
}

static Json idArray(const std::vector<long long>& v) {
    Json a = Json::makeArray();
    for (std::size_t i = 0; i < v.size(); ++i) a.push(Json::makeInt(v[i]));
    return a;
}

// "floor": {"nodes": 8, "edges": [[0,1,4], [1,2,3], ...]}   each edge is [u, v, length]
static void parseFloor(const Json& req, int& nodes, std::vector<FloorEdge>& edges) {
    const Json& floor = req.get("floor");
    nodes = getInt(floor, "nodes");
    const Json& list = floor.get("edges");
    edges.clear();
    edges.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& e = list.at(i);
        if (!e.isArray() || e.size() != 3)
            throw std::invalid_argument("each floor edge must be [u, v, length]");
        FloorEdge fe;
        fe.u = toInt(e.at(0).asInt(), "edge node");
        fe.v = toInt(e.at(1).asInt(), "edge node");
        fe.w = toInt(e.at(2).asInt(), "edge length");
        edges.push_back(fe);
    }
}

// "inventory": [{"sku": 1001, "node": 3}, ...]
static std::vector<SkuLocation> parseInventory(const Json& req) {
    std::vector<SkuLocation> inventory;
    const Json* list = req.find("inventory");
    if (list == nullptr) return inventory;
    inventory.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i) {
        SkuLocation s;
        s.sku = getInt(list->at(i), "sku");
        s.node = getInt(list->at(i), "node");
        inventory.push_back(s);
    }
    return inventory;
}

static Json okResponse(const char* op) {
    Json r = Json::makeObject();
    r.set("ok", Json::makeBool(true));
    r.set("op", Json::makeString(op));
    return r;
}

// ---------- operations ----------

static Json opPing() {
    Json r = okResponse("ping");
    r.set("engine", Json::makeString("warehouse-engine"));
    r.set("version", Json::makeString(ENGINE_VERSION));
    return r;
}

static Json opPlanTrip(const Json& req) {
    TripRequest t;
    parseFloor(req, t.nodes, t.edges);
    t.depot = getIntOr(req, "depot", 0);
    t.capacity = getInt(req, "capacity");
    t.maxCandidates = getIntOr(req, "max_candidates", PLAN_DEFAULT_CANDIDATES);
    t.inventory = parseInventory(req);

    const Json& orders = req.get("orders");
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const Json& o = orders.at(i);
        PlanOrder po;
        const Json* id = o.find("id");
        po.id = (id == nullptr || id->isNull()) ? static_cast<long long>(i) : id->asInt();
        po.sku = getInt(o, "sku");
        po.weight = getInt(o, "weight");
        po.value = getInt(o, "value");
        po.urgency = getIntOr(o, "urgency", 0);
        t.orders.push_back(po);
    }

    TripPlan plan = planTrip(t);

    Json r = okResponse("plan_trip");
    r.set("selected", idArray(plan.selected));
    r.set("deferred", idArray(plan.deferred));
    Json rejected = Json::makeArray();
    for (std::size_t i = 0; i < plan.rejected.size(); ++i) {
        Json one = Json::makeObject();
        one.set("id", Json::makeInt(plan.rejected[i].id));
        one.set("reason", Json::makeString(plan.rejected[i].reason));
        rejected.push(one);
    }
    r.set("rejected", rejected);
    Json stops = Json::makeArray();
    for (std::size_t i = 0; i < plan.stops.size(); ++i) {
        Json one = Json::makeObject();
        one.set("node", Json::makeInt(plan.stops[i].node));
        one.set("orders", idArray(plan.stops[i].orderIds));
        stops.push(one);
    }
    r.set("stops", stops);
    r.set("route", intArray(plan.route));
    r.set("distance", Json::makeInt(plan.distance));
    r.set("total_value", Json::makeInt(plan.totalValue));
    r.set("total_weight", Json::makeInt(plan.totalWeight));
    Json stats = Json::makeObject();
    stats.set("candidates", Json::makeInt(plan.candidates));
    stats.set("bb_nodes_explored", Json::makeInt(plan.boundNodesExplored));
    stats.set("bb_nodes_pruned", Json::makeInt(plan.boundNodesPruned));
    r.set("stats", stats);
    return r;
}

// turns one robot's trip into JSON (same fields plan_trip returns)
static Json tripToJson(const TripPlan& plan) {
    Json r = Json::makeObject();
    r.set("selected", idArray(plan.selected));
    r.set("deferred", idArray(plan.deferred));
    Json rejected = Json::makeArray();
    for (std::size_t i = 0; i < plan.rejected.size(); ++i) {
        Json one = Json::makeObject();
        one.set("id", Json::makeInt(plan.rejected[i].id));
        one.set("reason", Json::makeString(plan.rejected[i].reason));
        rejected.push(one);
    }
    r.set("rejected", rejected);
    Json stops = Json::makeArray();
    for (std::size_t i = 0; i < plan.stops.size(); ++i) {
        Json one = Json::makeObject();
        one.set("node", Json::makeInt(plan.stops[i].node));
        one.set("orders", idArray(plan.stops[i].orderIds));
        stops.push(one);
    }
    r.set("stops", stops);
    r.set("route", intArray(plan.route));
    r.set("distance", Json::makeInt(plan.distance));
    r.set("total_value", Json::makeInt(plan.totalValue));
    r.set("total_weight", Json::makeInt(plan.totalWeight));
    return r;
}

static Json opPlanFleet(const Json& req) {
    TripRequest t;
    parseFloor(req, t.nodes, t.edges);
    t.depot = getIntOr(req, "depot", 0);
    t.capacity = getInt(req, "capacity");
    t.maxCandidates = getIntOr(req, "max_candidates", PLAN_DEFAULT_CANDIDATES);
    t.inventory = parseInventory(req);

    const Json& orders = req.get("orders");
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const Json& o = orders.at(i);
        PlanOrder po;
        const Json* id = o.find("id");
        po.id = (id == nullptr || id->isNull()) ? static_cast<long long>(i) : id->asInt();
        po.sku = getInt(o, "sku");
        po.weight = getInt(o, "weight");
        po.value = getInt(o, "value");
        po.urgency = getIntOr(o, "urgency", 0);
        t.orders.push_back(po);
    }

    int robots = getIntOr(req, "robots", 1);
    FleetPlan fleet = planFleet(t, robots);

    Json r = okResponse("plan_fleet");
    r.set("robots", Json::makeInt(robots));
    Json trips = Json::makeArray();
    for (std::size_t i = 0; i < fleet.trips.size(); ++i) {
        Json one = tripToJson(fleet.trips[i]);
        Json st = Json::makeObject();
        st.set("candidates", Json::makeInt(fleet.trips[i].candidates));
        st.set("bb_nodes_explored", Json::makeInt(fleet.trips[i].boundNodesExplored));
        st.set("bb_nodes_pruned", Json::makeInt(fleet.trips[i].boundNodesPruned));
        one.set("stats", st);
        one.set("robot", Json::makeInt(static_cast<long long>(i) + 1));
        trips.push(one);
    }
    r.set("trips", trips);
    return r;
}

static Json opShortestPath(const Json& req) {
    int nodes = 0;
    std::vector<FloorEdge> edges;
    parseFloor(req, nodes, edges);
    if (nodes < 1 || nodes > PLAN_MAX_NODES) throw std::invalid_argument("floor.nodes must be between 1 and 2000000");
    Graph g(nodes);
    for (std::size_t i = 0; i < edges.size(); ++i) g.addEdge(edges[i].u, edges[i].v, edges[i].w);
    PathResult p = g.shortestPath(getInt(req, "from"), getInt(req, "to"));

    Json r = okResponse("shortest_path");
    r.set("reachable", Json::makeBool(p.reachable));
    r.set("distance", p.reachable ? Json::makeInt(p.distance) : Json());
    r.set("path", intArray(p.path));
    return r;
}

static Json opNetworkMst(const Json& req) {
    int nodes = 0;
    std::vector<FloorEdge> edges;
    parseFloor(req, nodes, edges);
    if (nodes < 1 || nodes > PLAN_MAX_NODES) throw std::invalid_argument("floor.nodes must be between 1 and 2000000");
    std::string algorithm = "kruskal";
    const Json* a = req.find("algorithm");
    if (a != nullptr && !a->isNull()) algorithm = a->asString();
    if (algorithm != "kruskal" && algorithm != "prim")
        throw std::invalid_argument("algorithm must be \"kruskal\" or \"prim\"");

    EdgeGraph g(nodes);
    for (std::size_t i = 0; i < edges.size(); ++i) g.addEdge(edges[i].u, edges[i].v, edges[i].w);
    MstResult m = (algorithm == "kruskal") ? g.kruskal() : g.prim();

    Json r = okResponse("network_mst");
    r.set("algorithm", Json::makeString(algorithm));
    r.set("spanning", Json::makeBool(m.spanning));
    r.set("components", Json::makeInt(m.components));
    r.set("total_weight", Json::makeInt(m.totalWeight));
    Json list = Json::makeArray();
    for (std::size_t i = 0; i < m.edges.size(); ++i) {
        Json e = Json::makeArray();
        e.push(Json::makeInt(m.edges[i].u)).push(Json::makeInt(m.edges[i].v)).push(Json::makeInt(m.edges[i].w));
        list.push(e);
    }
    r.set("edges", list);
    return r;
}

static void fillIndex(BPlusTree& index, const std::vector<SkuLocation>& inventory) {
    for (std::size_t i = 0; i < inventory.size(); ++i) index.insert(inventory[i].sku, inventory[i].node);
}

static Json opLookupSku(const Json& req) {
    BPlusTree index(64);
    fillIndex(index, parseInventory(req));
    int node = 0;
    bool found = index.find(getInt(req, "sku"), node);
    Json r = okResponse("lookup_sku");
    r.set("found", Json::makeBool(found));
    r.set("node", found ? Json::makeInt(node) : Json());
    return r;
}

static Json opSkuRange(const Json& req) {
    BPlusTree index(64);
    fillIndex(index, parseInventory(req));
    int limit = getIntOr(req, "limit", 1000);
    if (limit < 0) throw std::invalid_argument("limit cannot be negative");
    std::vector<std::pair<int, int> > hits = index.range(getInt(req, "lo"), getInt(req, "hi"));

    Json r = okResponse("sku_range");
    r.set("count", Json::makeInt(static_cast<long long>(hits.size())));
    r.set("truncated", Json::makeBool(static_cast<long long>(hits.size()) > limit));
    Json items = Json::makeArray();
    for (std::size_t i = 0; i < hits.size() && static_cast<int>(i) < limit; ++i) {
        Json one = Json::makeObject();
        one.set("sku", Json::makeInt(hits[i].first));
        one.set("node", Json::makeInt(hits[i].second));
        items.push(one);
    }
    r.set("items", items);
    return r;
}

static Json dispatch(const Json& req) {
    if (!req.isObject()) throw std::invalid_argument("the request must be a JSON object");
    const std::string& op = req.get("op").asString();
    if (op == "ping") return opPing();
    if (op == "plan_trip") return opPlanTrip(req);
    if (op == "plan_fleet") return opPlanFleet(req);
    if (op == "shortest_path") return opShortestPath(req);
    if (op == "network_mst") return opNetworkMst(req);
    if (op == "lookup_sku") return opLookupSku(req);
    if (op == "sku_range") return opSkuRange(req);
    throw std::invalid_argument("unknown op \"" + op + "\"");
}

static Json errorResponse(const std::string& message) {
    Json r = Json::makeObject();
    r.set("ok", Json::makeBool(false));
    r.set("error", Json::makeString(message));
    return r;
}

static void printUsage() {
    std::cout <<
        "warehouse engine " << ENGINE_VERSION << "\n"
        "Reads one JSON request and prints one JSON response.\n\n"
        "  engine_cli                      read the request from stdin\n"
        "  engine_cli --file request.json  read the request from a file\n"
        "  engine_cli --pretty             indent the response\n"
        "  engine_cli --help               this text\n\n"
        "Operations: ping, plan_trip, shortest_path, network_mst, lookup_sku, sku_range.\n"
        "See README.md for the request fields.\n";
}

int main(int argc, char** argv) {
    bool pretty = false;
    std::string filePath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(); return 0; }
        else if (arg == "--pretty") pretty = true;
        else if (arg == "--file" && i + 1 < argc) filePath = argv[++i];
        else { std::cerr << "unknown or incomplete argument: " << arg << "\nTry --help\n"; return 2; }
    }

    std::string text;
    if (!filePath.empty()) {
        std::ifstream in(filePath.c_str(), std::ios::binary);
        if (!in) {
            std::cout << errorResponse("cannot open file: " + filePath).dump(pretty ? 2 : -1) << "\n";
            return 1;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }

    Json response;
    bool ok = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    Json request;
    bool parsed = false;
    try {
        request = Json::parse(text);
        parsed = true;
    } catch (const JsonError& e) {
        response = errorResponse(std::string("invalid JSON: ") + e.what());
    }

    if (parsed) {
        try {
            response = dispatch(request);
            ok = true;
        } catch (const JsonError& e) {
            response = errorResponse(std::string("invalid request: ") + e.what());
        } catch (const std::invalid_argument& e) {
            response = errorResponse(std::string("invalid request: ") + e.what());
        } catch (const std::out_of_range& e) {
            response = errorResponse(std::string("invalid request: ") + e.what());
        } catch (const std::exception& e) {
            response = errorResponse(std::string("engine error: ") + e.what());
        }
    }
    if (ok) {
        long long micros = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started).count();
        response.set("elapsed_ms", Json::makeDouble(static_cast<double>(micros) / 1000.0));
    }
    std::cout << response.dump(pretty ? 2 : -1) << "\n";
    return ok ? 0 : 1;
}
