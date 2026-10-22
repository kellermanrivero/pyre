#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  tile_epsilon.hpp — an error limit of their own for some tiles of a local grid
// ═════════════════════════════════════════════════════════════════════════════
//
// A file can keep some tiles finer (or coarser) than the rest: each index entry
// has its own ε (docs/FORMAT.md). pyre-resample takes them from a text file:
//
//     # a comment
//     tile <i> <j> <epsilon>                               one tile, by its place
//     box <minLon> <minLat> <maxLon> <maxLat> <epsilon>    every tile that touches the box (degrees)
//
// ε is in vertical units (metres). When rules give one tile more than one ε,
// the smallest wins. Or from a reference heights pack: the tiles with an error
// of their own in its index.

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "local.hpp"

namespace pyre::tool {

/// Tile place (i, j) → the tile's error limit in quanta.
using TileEpsilons = std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t>;

/// Parses the text of a tile-epsilon file. `tileM` is a tile's side in metres.
/// Throws SourceError with the line number.
TileEpsilons parseTileEpsilons(const std::string& text, const LocalGrid& g, double tileM, double unit);

/// Reads a tile-epsilon file. Throws SourceError.
TileEpsilons readTileEpsilons(const std::string& path, const LocalGrid& g, double tileM, double unit);

/// The tiles of a reference heights pack (.alt) that have an error of their own. Throws SourceError.
TileEpsilons tileEpsilonsOfPack(const std::string& path, double unit);

/// Adds b to a; on a tile in both, the smaller ε stays.
void mergeTileEpsilons(TileEpsilons& a, const TileEpsilons& b);

}  // namespace pyre::tool
