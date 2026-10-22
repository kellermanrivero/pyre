#pragma once

// The tool's commands, and their small helpers.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "pyre/pyre.hpp"
#include "source.hpp"

namespace pyre::tool {

/// The command line: positional arguments, then --name value options and --flags.
struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    bool has(const std::string& k) const { return options.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
    double number(const std::string& k, double def) const;
    std::uint32_t count(const std::string& k, std::uint32_t def) const;
};

/// The options that take a value; the rest are flags.
bool takesValue(const std::string& option);

int cmdInfo(const Args& a);
int cmdEncode(const Args& a);
int cmdDecode(const Args& a);
int cmdVerify(const Args& a);
int cmdBench(const Args& a);

/// Runs f(k) for k in [0, n) on `threads` threads; the first exception wins.
void parallelFor(std::size_t n, unsigned threads, const std::function<void(std::size_t)>& f);

unsigned threadsOf(const Args& a);

inline double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

}  // namespace pyre::tool
