// json.hpp - a small JSON value type with a parser and a writer.
//
// Purpose: lets the C++ engine read a JSON request on stdin and write a JSON
// response on stdout, so Python (FastAPI) can talk to it without any library.
//
// Supports the full JSON grammar: null, true/false, numbers, strings (with
// \uXXXX escapes and surrogate pairs), arrays, objects. Rejects malformed
// input with a JsonError that says where it went wrong.
//
// Numbers written without a fraction or exponent are kept as exact 64-bit
// integers (so ids and distances never lose precision); everything else is a double.
// Objects keep the order keys were inserted, which makes output readable.
//
// Written to compile with older compilers (C++11).

#pragma once
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Null), bool_(false), num_(0.0), int_(0), isInt_(false) {}

    // ---- constructors by kind ----
    static Json makeBool(bool b) { Json j; j.type_ = Bool; j.bool_ = b; return j; }
    static Json makeInt(long long v) {
        Json j; j.type_ = Number; j.isInt_ = true; j.int_ = v; j.num_ = static_cast<double>(v); return j;
    }
    static Json makeDouble(double v) { Json j; j.type_ = Number; j.num_ = v; return j; }
    static Json makeString(const std::string& s) { Json j; j.type_ = String; j.str_ = s; return j; }
    static Json makeArray() { Json j; j.type_ = Array; return j; }
    static Json makeObject() { Json j; j.type_ = Object; return j; }

    // ---- inspection ----
    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isBool() const { return type_ == Bool; }
    bool isNumber() const { return type_ == Number; }
    bool isString() const { return type_ == String; }
    bool isArray() const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }

    const char* typeName() const {
        switch (type_) {
            case Null: return "null";
            case Bool: return "boolean";
            case Number: return "number";
            case String: return "string";
            case Array: return "array";
            default: return "object";
        }
    }

    bool asBool() const { expect(Bool); return bool_; }
    double asDouble() const { expect(Number); return num_; }
    const std::string& asString() const { expect(String); return str_; }

    // The number as an exact integer. Throws if it has a fraction/exponent or is out of range.
    long long asInt() const {
        expect(Number);
        if (!isInt_) throw JsonError("expected a whole number but found " + dumpNumber());
        return int_;
    }

    // Element count of an array or object.
    std::size_t size() const {
        if (type_ == Array) return arr_.size();
        if (type_ == Object) return obj_.size();
        throw JsonError(std::string("size() needs an array or object, not ") + typeName());
    }

    const Json& at(std::size_t i) const {
        expect(Array);
        if (i >= arr_.size()) throw JsonError("array index out of range");
        return arr_[i];
    }

    // Object lookup. find() returns nullptr when the key is missing.
    const Json* find(const std::string& key) const {
        expect(Object);
        for (std::size_t i = 0; i < obj_.size(); ++i)
            if (obj_[i].first == key) return &obj_[i].second;
        return nullptr;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }
    const Json& get(const std::string& key) const {
        const Json* p = find(key);
        if (p == nullptr) throw JsonError("missing required field \"" + key + "\"");
        return *p;
    }

    // ---- building ----
    Json& push(const Json& v) { expect(Array); arr_.push_back(v); return *this; }
    Json& set(const std::string& key, const Json& v) {
        expect(Object);
        for (std::size_t i = 0; i < obj_.size(); ++i)
            if (obj_[i].first == key) { obj_[i].second = v; return *this; }
        obj_.push_back(std::make_pair(key, v));
        return *this;
    }

    // ---- output ----
    // indent < 0: compact one-liner. indent >= 0: pretty-printed with that many spaces per level.
    std::string dump(int indent = -1) const {
        std::string out;
        write(out, indent, 0);
        return out;
    }

    // ---- input ----
    static Json parse(const std::string& text) {
        Parser p(text);
        return p.document();
    }

private:
    Type type_;
    bool bool_;
    double num_;
    long long int_;
    bool isInt_;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json> > obj_;

    void expect(Type t) const {
        if (type_ != t) {
            Json probe; probe.type_ = t;
            throw JsonError(std::string("expected ") + probe.typeName() + " but found " + typeName());
        }
    }

    std::string dumpNumber() const {
        if (isInt_) {
            std::ostringstream s;
            s << int_;
            return s.str();
        }
        // Use 15 significant digits when that reads back to the exact same double
        // (so 0.075 prints as 0.075), otherwise 17, which always round-trips.
        std::ostringstream shortForm;
        shortForm << std::setprecision(15) << num_;
        if (std::strtod(shortForm.str().c_str(), nullptr) == num_) return shortForm.str();
        std::ostringstream longForm;
        longForm << std::setprecision(17) << num_;
        return longForm.str();
    }

    static void writeString(std::string& out, const std::string& s) {
        static const char* hex = "0123456789abcdef";
        out += '"';
        for (std::size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 0xF];
                    } else {
                        out += static_cast<char>(c);     // UTF-8 bytes pass through unchanged
                    }
            }
        }
        out += '"';
    }

    static void newline(std::string& out, int indent, int level) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
    }

    void write(std::string& out, int indent, int level) const {
        switch (type_) {
            case Null: out += "null"; break;
            case Bool: out += bool_ ? "true" : "false"; break;
            case Number: out += dumpNumber(); break;
            case String: writeString(out, str_); break;
            case Array:
                if (arr_.empty()) { out += "[]"; break; }
                out += '[';
                for (std::size_t i = 0; i < arr_.size(); ++i) {
                    if (i > 0) out += ',';
                    newline(out, indent, level + 1);
                    arr_[i].write(out, indent, level + 1);
                }
                newline(out, indent, level);
                out += ']';
                break;
            case Object:
                if (obj_.empty()) { out += "{}"; break; }
                out += '{';
                for (std::size_t i = 0; i < obj_.size(); ++i) {
                    if (i > 0) out += ',';
                    newline(out, indent, level + 1);
                    writeString(out, obj_[i].first);
                    out += indent < 0 ? ":" : ": ";
                    obj_[i].second.write(out, indent, level + 1);
                }
                newline(out, indent, level);
                out += '}';
                break;
        }
    }

    // Recursive-descent parser: one small function per JSON construct.
    class Parser {
    public:
        explicit Parser(const std::string& text) : s_(text), pos_(0) {}

        Json document() {
            skipSpace();
            Json v = value(0);
            skipSpace();
            if (pos_ != s_.size()) fail("unexpected data after the JSON value");
            return v;
        }

    private:
        const std::string& s_;
        std::size_t pos_;

        static const int MAX_DEPTH = 100;     // stops "[[[[[..." from overflowing the stack

        void fail(const std::string& message) const {
            std::ostringstream os;
            os << message << " (at character " << pos_ << ")";
            throw JsonError(os.str());
        }

        void skipSpace() {
            while (pos_ < s_.size() &&
                   (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
                ++pos_;
        }

        bool matchWord(const char* word) {
            std::size_t n = 0;
            while (word[n] != '\0') ++n;
            if (s_.compare(pos_, n, word) != 0) return false;
            pos_ += n;
            return true;
        }

        Json value(int depth) {
            if (depth > MAX_DEPTH) fail("JSON is nested too deeply");
            if (pos_ >= s_.size()) fail("unexpected end of input");
            char c = s_[pos_];
            if (c == '{') return object(depth);
            if (c == '[') return array(depth);
            if (c == '"') return Json::makeString(string());
            if (c == 't') { if (matchWord("true")) return Json::makeBool(true); fail("invalid literal"); }
            if (c == 'f') { if (matchWord("false")) return Json::makeBool(false); fail("invalid literal"); }
            if (c == 'n') { if (matchWord("null")) return Json(); fail("invalid literal"); }
            if (c == '-' || (c >= '0' && c <= '9')) return number();
            fail("unexpected character");
            return Json();
        }

        Json object(int depth) {
            ++pos_;                                   // skip '{'
            Json result = Json::makeObject();
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return result; }
            for (;;) {
                skipSpace();
                if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected a string key");
                std::string key = string();
                skipSpace();
                if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':' after key");
                ++pos_;
                skipSpace();
                result.set(key, value(depth + 1));
                skipSpace();
                if (pos_ >= s_.size()) fail("unterminated object");
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == '}') { ++pos_; return result; }
                fail("expected ',' or '}'");
            }
        }

        Json array(int depth) {
            ++pos_;                                   // skip '['
            Json result = Json::makeArray();
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return result; }
            for (;;) {
                skipSpace();
                result.push(value(depth + 1));
                skipSpace();
                if (pos_ >= s_.size()) fail("unterminated array");
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == ']') { ++pos_; return result; }
                fail("expected ',' or ']'");
            }
        }

        unsigned hex4() {
            if (pos_ + 4 > s_.size()) fail("incomplete \\u escape");
            unsigned v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = s_[pos_++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
                else fail("invalid hex digit in \\u escape");
            }
            return v;
        }

        static void appendUtf8(std::string& out, unsigned cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        std::string string() {
            ++pos_;                                   // skip opening quote
            std::string out;
            for (;;) {
                if (pos_ >= s_.size()) fail("unterminated string");
                unsigned char c = static_cast<unsigned char>(s_[pos_++]);
                if (c == '"') return out;
                if (c < 0x20) fail("control character inside string");
                if (c != '\\') { out += static_cast<char>(c); continue; }
                if (pos_ >= s_.size()) fail("unterminated escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate: needs a low one next
                            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                                fail("high surrogate without a following low surrogate");
                            pos_ += 2;
                            unsigned low = hex4();
                            if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("unexpected low surrogate");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail("invalid escape sequence");
                }
            }
        }

        Json number() {
            std::size_t start = pos_;
            if (s_[pos_] == '-') ++pos_;
            if (pos_ >= s_.size()) fail("incomplete number");
            if (s_[pos_] == '0') {
                ++pos_;
            } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
                while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            } else {
                fail("invalid number");
            }
            bool isInteger = true;
            if (pos_ < s_.size() && s_[pos_] == '.') {
                isInteger = false;
                ++pos_;
                if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("digits expected after '.'");
                while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            }
            if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
                isInteger = false;
                ++pos_;
                if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
                if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') fail("digits expected in exponent");
                while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            }
            std::string text = s_.substr(start, pos_ - start);
            if (isInteger) {
                errno = 0;
                long long v = std::strtoll(text.c_str(), nullptr, 10);
                if (errno != ERANGE) return Json::makeInt(v);
                // too big for 64 bits: fall through and keep it as a double
            }
            return Json::makeDouble(std::strtod(text.c_str(), nullptr));
        }
    };
};
