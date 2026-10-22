#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  overlay.hpp — rasters on the local grid whose values replace the samples
// ═════════════════════════════════════════════════════════════════════════════
//
// pyre-resample --overlay <raster>: before a tile is encoded, each node that an
// overlay covers with a value takes that value in place of the sample. The
// error limit then holds against the overlay's value. Uses: the smoothed
// ground of an airfield, a filled hole, a corrected area.
//
// An overlay must lie on the grid's lattice:
//   - its pixel spacing is the grid's step, in both directions;
//   - each pixel's sample point is a node (PixelIsPoint: the tie point; PixelIsArea:
//     the cell's centre);
//   - its CRS is the same local grid (a "pyre local" WKT with the same centre),
//     or not given at all. Another CRS is refused.
// `pyre decode` of a pyre-resample file writes such a raster, so a tool can
// decode an area, change it and give it back as an overlay.
//
// A pixel with no data (the no-data value, NaN or infinite) leaves the sample
// as it is. Where overlays meet, the last one given wins.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "local.hpp"
#include "source.hpp"

namespace pyre::tool {

class Overlay {
public:
    /// Opens the rasters and checks that they lie on the grid. Throws SourceError.
    Overlay(const std::vector<std::string>& paths, const LocalGrid& g, double step);
    ~Overlay();

    bool empty() const { return layers_.empty(); }

    /// Replaces the values of the square of side × side nodes from global node
    /// (I0, J0) (`values`: row J major, south row first) where an overlay has
    /// data. Returns the count of nodes set. Thread-safe.
    std::size_t apply(std::int64_t I0, std::int64_t J0, std::uint32_t side, std::vector<double>& values) const;

private:
    struct Layer {
        std::unique_ptr<Source> source;
        std::int64_t firstI = 0, topJ = 0;   // the node of pixel (0, 0): its column, and the row of nodes at the top
        std::uint32_t width = 0, height = 0;
    };
    std::vector<Layer> layers_;
};

}  // namespace pyre::tool
