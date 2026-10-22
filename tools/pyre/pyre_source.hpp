#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  pyre_source.hpp — one grid of a .pyre file, read as a source raster
// ═════════════════════════════════════════════════════════════════════════════
//
// With this reader a pyre file can stand in for the rasters it was made from.
// For example, an archive of MDT02 sheets made with `pyre encode` (one grid a
// sheet, on the sheet's own lattice) is a source for `pyre-resample`, and the
// original sheets are no longer needed. Each value is within the file's ε (plus
// q/2) of the original.
//
// The path is "file.pyre" (a file with one grid) or "file.pyre#K" (grid K).
// Pixel (0, 0) is the grid's north-west node; the georeference is the grid's.
// A value is the float32 nearest to height · unit, as a float32 raster holds
// it. So a float32 source whose values are whole quanta (the MDT02's are whole
// mm), encoded with that unit and ε = 0, reads back bit for bit.
// A node with no data (a hole, or a tile not in the file) reads as NaN. The
// reader decodes whole tiles and keeps the last ones (an LRU cache); it is safe
// on many threads.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "source.hpp"

namespace pyre::tool {

class PyreSource final : public Source {
public:
    static constexpr std::size_t kDefaultCacheTiles = 16;

    /// Opens "file.pyre" or "file.pyre#K". With `predictedHoles`, a hole reads
    /// as the value the decoder puts there (its prediction from the nodes round
    /// it) instead of NaN; a tile that is not in the file is still NaN.
    /// Throws SourceError.
    static std::unique_ptr<PyreSource> open(const std::string& path, std::size_t cacheTiles = kDefaultCacheTiles,
                                            bool predictedHoles = false);
    /// The count of grids in a .pyre file. Throws SourceError.
    static std::size_t gridCount(const std::string& file);

    ~PyreSource() override;
    PyreSource(const PyreSource&) = delete;
    PyreSource& operator=(const PyreSource&) = delete;

    const SourceInfo& info() const override;
    void readWindow(std::uint32_t col0, std::uint32_t row0, std::uint32_t width, std::uint32_t height, double* out) const override;

    struct Impl;

private:
    explicit PyreSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace pyre::tool
