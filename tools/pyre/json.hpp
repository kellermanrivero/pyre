#pragma once

// A small JSON writer for --json: one object, built in order.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pyre::tool {

class Json {
public:
    Json() { s_ = "{"; }

    Json& add(const std::string& key, const std::string& v) { return raw(key, quote(v)); }
    Json& add(const std::string& key, const char* v) { return raw(key, quote(v)); }
    Json& add(const std::string& key, bool v) { return raw(key, v ? "true" : "false"); }
    Json& add(const std::string& key, double v) { return raw(key, number(v)); }
    Json& add(const std::string& key, std::int64_t v) { return raw(key, std::to_string(v)); }
    Json& add(const std::string& key, std::uint64_t v) { return raw(key, std::to_string(v)); }
    Json& add(const std::string& key, int v) { return raw(key, std::to_string(v)); }
    Json& add(const std::string& key, unsigned v) { return raw(key, std::to_string(v)); }
    Json& add(const std::string& key, const Json& v) { return raw(key, v.str()); }
    Json& add(const std::string& key, const std::vector<Json>& v) {
        std::string a = "[";
        for (std::size_t k = 0; k < v.size(); ++k) a += (k ? "," : "") + v[k].str();
        return raw(key, a + "]");
    }
    Json& add(const std::string& key, const std::vector<double>& v) {
        std::string a = "[";
        for (std::size_t k = 0; k < v.size(); ++k) a += (k ? "," : "") + number(v[k]);
        return raw(key, a + "]");
    }

    std::string str() const { return s_ + "}"; }

    static std::string number(double v) {
        if (!std::isfinite(v)) return "null";
        char b[32];
        std::snprintf(b, sizeof b, "%.10g", v);
        return b;
    }
    static std::string quote(const std::string& v) {
        std::string o = "\"";
        for (char c : v) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        o += b;
                    } else {
                        o += c;
                    }
            }
        }
        return o + "\"";
    }

private:
    Json& raw(const std::string& key, const std::string& v) {
        if (s_.size() > 1) s_ += ",";
        s_ += quote(key) + ":" + v;
        return *this;
    }
    std::string s_;
};

}  // namespace pyre::tool
