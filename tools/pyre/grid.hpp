#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  grid.hpp — from a source raster to a pyre grid and its tiles
// ═════════════════════════════════════════════════════════════════════════════
//
// v0.1 does not resample: each pyre node is one source pixel. A source is one
// grid of the file. Its node (I, J) is the pixel at column I and row H − 1 − J
// (J grows northwards, rows go south), so node (0, 0) is the south-west pixel.
// Tile (ti, tj) holds nodes ti·S … ti·S + S and tj·S … tj·S + S; nodes past
// the raster's edge are holes. Tiles start at the south-west pixel, so a
// sheet of W × H pixels has ⌈(W − 1)/S⌉ × ⌈(H − 1)/S⌉ tiles.
//
// A later resampler (onto the AirKid grid, for example) replaces only
// readTile: the reader below it and the codec above it do not change.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "pyre/container.hpp"
#include "source.hpp"

namespace pyre::tool {

/// The grid of the file for one source.
GridInfo gridOf(const SourceInfo& s, const Params& p);

/// A source value in quanta: round(v / unit). Throws if it is out of range.
std::int32_t toQuanta(double v, double unit);

/// The error limit in quanta for ε in metres: the largest D with D·unit ≤ ε.
std::int32_t errorQuanta(double epsilon, double unit);

/// Reads tile (ti, tj) of a source: `x` gets the heights in quanta, `valid`
/// 1 or 0 for each node. Returns the count of valid nodes. `buf` is a scratch
/// buffer. Thread-safe if the source is.
std::size_t readTile(const Source& src, const Params& p, double unit, std::uint32_t ti, std::uint32_t tj,
                     std::vector<std::int32_t>& x, std::vector<std::uint8_t>& valid, std::vector<double>& buf);

/// Reads the float source values of tile (ti, tj) (NaN where no-data or
/// outside), for verify.
void readTileValues(const Source& src, const Params& p, std::uint32_t ti, std::uint32_t tj, std::vector<double>& v);

/// A rectangle in CRS units.
struct BBox {
    double minX = -INFINITY, minY = -INFINITY, maxX = INFINITY, maxY = INFINITY;
    bool all() const { return std::isinf(minX) && std::isinf(minY) && std::isinf(maxX) && std::isinf(maxY); }
};

/// True if tile (ti, tj) of grid g touches the box.
bool tileTouches(const GridInfo& g, const Params& p, std::int32_t ti, std::int32_t tj, const BBox& b);

std::string baseName(const std::string& path);

}  // namespace pyre::tool
