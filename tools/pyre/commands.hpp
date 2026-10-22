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

#include "grid.hpp"
#include "pyre/pyre.hpp"
#include "source.hpp"

namespace pyre::tool {

/// The command line: positional arguments, then --name value options and --flags.
struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;            ///< the last value of each option
    std::map<std::string, std::vector<std::string>> all;   ///< every value of each option, in order

    bool has(const std::string& k) const { return options.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
    std::vector<std::string> values(const std::string& k) const {
        auto it = all.find(k);
        return it == all.end() ? std::vector<std::string>{} : it->second;
    }
    double number(const std::string& k, double def) const;
    std::uint32_t count(const std::string& k, std::uint32_t def) const;
};

/// The options that take a value; the rest are flags.
bool takesValue(const std::string& option);

/// Parses argv[first …]: "-o" is "--output"; "--name" takes the next argument
/// if takes(name), else it is a flag. Throws std::runtime_error.
Args parseArgs(int argc, char** argv, int first, const std::function<bool(const std::string&)>& takes);

/// The codec parameters from --tile, --block, --base and --block-levels. Throws.
Params paramsOf(const Args& a);
/// A box option "minX,minY,maxX,maxY"; all space if the option is not given. Throws.
BBox bboxOf(const Args& a, const std::string& name = "bbox");
/// Throws std::runtime_error with `what` if s is not Ok.
void need(Status s, const char* what);

int cmdInfo(const Args& a);
int cmdEncode(const Args& a);
int cmdDecode(const Args& a);
int cmdVerify(const Args& a);
int cmdBench(const Args& a);
int cmdCompare(const Args& a);

// ── Encoding many tiles into one file ─────────────────────────────────────────

struct TileJob {
    std::uint32_t grid = 0;
    std::int32_t ti = 0, tj = 0;
    std::int32_t d = -1;   ///< the tile's own error limit in quanta; −1: the file's

    std::int32_t errorOr(std::int32_t fileD) const { return d >= 0 ? d : fileD; }
};

/// One tile's input: heights in quanta, a validity byte a node, and (if the
/// reader gives them) the float values, NaN where no data.
struct TileData {
    std::vector<std::int32_t> x;
    std::vector<std::uint8_t> valid;
    std::vector<double> values;
};

/// Fills the tile of a job. Returns the count of valid nodes; 0: the tile is not stored.
using TileReader = std::function<std::size_t(const TileJob&, TileData&)>;
/// Called on the worker thread after a tile is encoded, with the heights that a
/// decoder gets. May throw. May be empty.
using TileCheck = std::function<void(const TileJob&, const TileData&, const EncodedTile&, const std::vector<std::int32_t>& recon)>;

struct EncodeTotals {
    std::size_t tiles = 0, emptyTiles = 0;
    std::uint64_t validNodes = 0, bytes = 0;
    double seconds = 0;
};

/// Reads, encodes and writes the jobs' tiles on `threads` threads. The file
/// holds the tiles in the order of the jobs, so the same input gives the same
/// bytes. Throws std::runtime_error.
EncodeTotals encodeToFile(const FileInfo& info, const std::vector<TileJob>& jobs, const TileReader& read,
                          const TileCheck& check, unsigned threads, const std::string& path);

/// Runs f(k) for k in [0, n) on `threads` threads; the first exception wins.
void parallelFor(std::size_t n, unsigned threads, const std::function<void(std::size_t)>& f);

unsigned threadsOf(const Args& a);

inline double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

}  // namespace pyre::tool
