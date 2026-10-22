#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  surface.hpp — layer kind 1: the height of objects above the ground
// ═════════════════════════════════════════════════════════════════════════════
//
// The second pass of `pyre encode … --layer-of <file.pyre> --kind 1`: from the
// file's own heights (the ground) and surface sources (a digital surface model
// with buildings and trees, in degrees; for example the MDS02):
//
//   1. At each node of the file's grid: h = surface − ground. The ground is the
//      decoded height; under a hole of the heights it is the value the decoder
//      puts there (its prediction), so a building on a hole still gets a height.
//      The surface is sampled at the node with the rule of resample.hpp (the
//      deepest sheet, bilinear): its sheets need not share the grid's lattice.
//   2. Layer node (i, j) of a tile is node (stride·i, stride·j). Its value is the
//      largest h of the stride × stride nodes round it: node offsets
//      −stride/2 … stride − stride/2 − 1 in each direction (the node itself for
//      stride 1). With no h there, the layer node has no data.
//   3. That height in quanta of `unit` (the step): 0 under `min` (the ground and
//      low objects); else the nearest whole count of steps, at least 1. With
//      unit = min = 3 m: under 3 m → 0; then 3, 6, 9 … m, within 1.5 m.
//
// Each value comes from the data at its own place, so the two tiles that share
// a side compute the same values there.

#include <cstdint>
#include <optional>
#include <vector>

#include "grid.hpp"
#include "local.hpp"
#include "resample.hpp"

namespace pyre::tool {

struct SurfaceOptions {
    double unit = 3;            ///< metres a step (the layer's quantum)
    double min = 3;             ///< heights under it are 0
    std::uint32_t stride = 2;   ///< grid nodes a layer node, in each direction
};

/// A height above the ground, in steps.
std::int32_t surfaceSteps(double h, const SurfaceOptions& o);

/// Where the nodes of a grid are, in degrees: a grid in a geographic CRS, or a
/// local grid of pyre-resample.
class NodePlaces {
public:
    /// Throws std::runtime_error for a grid in any other CRS.
    explicit NodePlaces(const GridInfo& g);
    double lon(std::int64_t I) const;
    double lat(std::int64_t J) const;

private:
    GridInfo g_;
    std::optional<LocalGrid> local_;
    bool area_ = false;
};

class SurfaceMaker {
public:
    SurfaceMaker(const SheetSet& surface, SurfaceOptions o) : surface_(surface), o_(o) {}
    const SurfaceOptions& options() const { return o_; }

    /// The layer tile (ti, tj) of grid g. `ground` reads the grid: its pixel
    /// (0, 0) is node (tileI0·S, tileJ0·S + height − 1), as PyreSource gives it.
    /// x gets the values in steps, valid 1 or 0, for (S/stride + 1)² nodes.
    /// Returns the count of valid nodes. Thread-safe.
    std::size_t make(const Source& ground, const GridInfo& g, const NodePlaces& places, const Params& p, std::int32_t ti,
                     std::int32_t tj, std::vector<std::int32_t>& x, std::vector<std::uint8_t>& valid) const;

private:
    const SheetSet& surface_;
    SurfaceOptions o_;
};

}  // namespace pyre::tool
