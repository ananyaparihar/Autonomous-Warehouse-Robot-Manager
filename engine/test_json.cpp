// test_json.cpp - tests for json.hpp.
// Build:  g++ -std=c++14 -O2 test_json.cpp -o test_json
// Run:    .\test_json.exe

#include "json.hpp"
#include <iostream>

static int passed = 0, failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++passed; }                                              \
        else { ++failed; std::cout << "FAIL line " << __LINE__ << ": "       \
                                   << #cond << "\n"; }                       \
    } while (0)

static bool rejects(const std::string& text) {
    try { Json::parse(text); } catch (const JsonError&) { return true; }
    return false;
}

void test_scalars() {
    CHECK(Json::parse("null").isNull());
    CHECK(Json::parse("true").asBool() == true);
    CHECK(Json::parse("false").asBool() == false);
    CHECK(Json::parse("  \n\t 42 \r\n").asInt() == 42);
    CHECK(Json::parse("-7").asInt() == -7);
    CHECK(Json::parse("0").asInt() == 0);
    CHECK(Json::parse("9223372036854775807").asInt() == 9223372036854775807LL);   // exact, no double rounding
    CHECK(Json::parse("3.5").asDouble() == 3.5);
    CHECK(Json::parse("1e3").asDouble() == 1000.0);
    CHECK(Json::parse("-2.5E-1").asDouble() == -0.25);
    CHECK(Json::parse("\"hi\"").asString() == "hi");
}

void test_integer_strictness() {
    bool threwFraction = false, threwExp = false, threwBig = false;
    try { Json::parse("3.5").asInt(); } catch (const JsonError&) { threwFraction = true; }
    try { Json::parse("1e3").asInt(); } catch (const JsonError&) { threwExp = true; }
    try { Json::parse("99999999999999999999").asInt(); } catch (const JsonError&) { threwBig = true; }
    CHECK(threwFraction);
    CHECK(threwExp);
    CHECK(threwBig);                                   // too large for 64 bits: kept as a double, not an int
    CHECK(Json::parse("99999999999999999999").isNumber());
}

void test_strings_and_escapes() {
    CHECK(Json::parse("\"a\\nb\\t\\\"q\\\" \\\\ \\/\"").asString() == "a\nb\t\"q\" \\ /");
    CHECK(Json::parse("\"\\u0041\"").asString() == "A");
    CHECK(Json::parse("\"\\u00e9\"").asString() == "\xC3\xA9");                 // e-acute as UTF-8
    CHECK(Json::parse("\"\\u20ac\"").asString() == "\xE2\x82\xAC");             // euro sign
    CHECK(Json::parse("\"\\ud83d\\ude00\"").asString() == "\xF0\x9F\x98\x80");  // surrogate pair -> one emoji
    CHECK(Json::parse("\"caf\xC3\xA9\"").asString() == "caf\xC3\xA9");          // raw UTF-8 passes through
    CHECK(Json::parse("\"\"").asString().empty());
}

void test_arrays_and_objects() {
    Json a = Json::parse("[1, 2, [3, 4], {\"x\": null}]");
    CHECK(a.isArray() && a.size() == 4);
    CHECK(a.at(0).asInt() == 1);
    CHECK(a.at(2).at(1).asInt() == 4);
    CHECK(a.at(3).get("x").isNull());

    Json o = Json::parse("{\"b\": 2, \"a\": 1, \"list\": []}");
    CHECK(o.isObject() && o.size() == 3);
    CHECK(o.get("a").asInt() == 1);
    CHECK(o.has("list") && o.get("list").size() == 0);
    CHECK(!o.has("zzz"));
    CHECK(o.find("zzz") == nullptr);
    CHECK(Json::parse("[]").size() == 0);
    CHECK(Json::parse("{}").size() == 0);
}

void test_type_errors() {
    bool wrongType = false, missing = false, badIndex = false, badSize = false;
    try { Json::parse("\"x\"").asInt(); } catch (const JsonError&) { wrongType = true; }
    try { Json::parse("{}").get("nope"); } catch (const JsonError&) { missing = true; }
    try { Json::parse("[1]").at(5); } catch (const JsonError&) { badIndex = true; }
    try { Json::parse("5").size(); } catch (const JsonError&) { badSize = true; }
    CHECK(wrongType);
    CHECK(missing);
    CHECK(badIndex);
    CHECK(badSize);
}

void test_rejects_bad_input() {
    const char* bad[] = {
        "", "   ", "{", "}", "[", "]", "[1,]", "[,1]", "{\"a\":1,}", "{\"a\"}", "{\"a\":}",
        "{a:1}", "{'a':1}", "[1 2]", "01", "-", "1.", ".5", "+1", "1e", "1e+", "nul", "tru", "True",
        "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "\"\\u12G4\"", "\"\\ud800\"", "\"\\udc00\"",
        "{} x", "1 2", "[1]]", "NaN", "Infinity"
    };
    bool allRejected = true;
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (!rejects(bad[i])) { allRejected = false; std::cout << "  accepted bad input: " << bad[i] << "\n"; }
    }
    CHECK(allRejected);
}

void test_deep_nesting_is_rejected_not_crashed() {
    std::string deep(5000, '[');
    deep += std::string(5000, ']');
    CHECK(rejects(deep));
    std::string fine(50, '[');
    fine += std::string(50, ']');
    CHECK(!rejects(fine));
}

void test_dump_compact_and_pretty() {
    Json o = Json::makeObject();
    o.set("name", Json::makeString("a\"b"));
    o.set("n", Json::makeInt(-5));
    o.set("pi", Json::makeDouble(0.5));
    o.set("ok", Json::makeBool(true));
    o.set("nothing", Json());
    Json arr = Json::makeArray();
    arr.push(Json::makeInt(1)).push(Json::makeInt(2));
    o.set("list", arr);
    o.set("empty", Json::makeArray());
    CHECK(o.dump() == "{\"name\":\"a\\\"b\",\"n\":-5,\"pi\":0.5,\"ok\":true,\"nothing\":null,"
                      "\"list\":[1,2],\"empty\":[]}");
    CHECK(o.dump(2).find("\n  \"n\": -5") != std::string::npos);
    CHECK(Json::parse(o.dump(2)).dump() == o.dump());       // pretty output parses back to the same thing
}

void test_set_replaces_and_keeps_order() {
    Json o = Json::makeObject();
    o.set("a", Json::makeInt(1));
    o.set("b", Json::makeInt(2));
    o.set("a", Json::makeInt(9));                           // replace, don't duplicate
    CHECK(o.size() == 2);
    CHECK(o.dump() == "{\"a\":9,\"b\":2}");
    CHECK(Json::parse("{\"x\":1,\"x\":2}").get("x").asInt() == 2);   // duplicate key in input: last one wins
}

void test_round_trip() {
    std::string text = "{\"floor\":{\"nodes\":4,\"edges\":[[0,1,5],[1,2,3]]},\"s\":\"t\\u00e9\\n\","
                       "\"big\":9007199254740993,\"neg\":-1.25,\"flags\":[true,false,null]}";
    Json j = Json::parse(text);
    CHECK(j.get("big").asInt() == 9007199254740993LL);      // 2^53 + 1: would be corrupted by a double
    Json again = Json::parse(j.dump());
    CHECK(again.dump() == j.dump());
    CHECK(again.get("floor").get("edges").at(1).at(2).asInt() == 3);
}

void test_control_characters_are_escaped_on_output() {
    Json s = Json::makeString(std::string("a\x01" "b\x1f", 4));
    CHECK(s.dump() == "\"a\\u0001b\\u001f\"");
    CHECK(Json::parse(s.dump()).asString() == s.asString());
}

void test_double_formatting_is_short_and_exact() {
    CHECK(Json::makeDouble(0.075).dump() == "0.075");
    CHECK(Json::makeDouble(0.1).dump() == "0.1");
    CHECK(Json::makeDouble(-2.5).dump() == "-2.5");
    CHECK(Json::makeDouble(1000000.0).dump() == "1000000");
    double third = 1.0 / 3.0;                                   // needs all 17 digits to survive
    CHECK(Json::parse(Json::makeDouble(third).dump()).asDouble() == third);
    double tiny = 1.2345678901234567e-300;
    CHECK(Json::parse(Json::makeDouble(tiny).dump()).asDouble() == tiny);
}

int main() {
    test_double_formatting_is_short_and_exact();
    test_scalars();
    test_integer_strictness();
    test_strings_and_escapes();
    test_arrays_and_objects();
    test_type_errors();
    test_rejects_bad_input();
    test_deep_nesting_is_rejected_not_crashed();
    test_dump_compact_and_pretty();
    test_set_replaces_and_keeps_order();
    test_round_trip();
    test_control_characters_are_escaped_on_output();
    std::cout << "passed: " << passed << "  failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
