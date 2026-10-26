#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  tiff.hpp — the GeoTIFF / COG reader and a small GeoTIFF writer
// ═════════════════════════════════════════════════════════════════════════════
//
// The reader:
// - TIFF and BigTIFF, little-endian (II) and big-endian (MM).
// - Strips and tiles. One band (SamplesPerPixel 1).
// - Compression: none (1), LZW (5), Deflate (8 and 32946), and ZSTD (50000)
//   when the build has libzstd (PYRE_WITH_ZSTD).
// - Predictor: 1, 2 (horizontal differencing) and 3 (floating point).
// - Samples: uint8, int16, uint16, int32, uint32, float32, float64.
// - It uses the first full-resolution image and ignores overviews and masks.
// - readWindow reads only the tiles or strips that the window touches. It uses
//   pread (no shared file position) and a small LRU cache of decoded blocks, so
//   it is safe to call from many threads at once.
// - A damaged or truncated file gives SourceError, never a crash.
//
// The writer makes a plain GeoTIFF: float32, one band, 256 × 256 tiles,
// Deflate with predictor 3. Classic TIFF when it fits, else BigTIFF.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "source.hpp"

namespace pyre::tool {

/// How the image is stored in the file (for `pyre info` and for tests).
struct TiffLayout {
    bool bigTiff = false;
    bool bigEndian = false;
    bool tiled = false;
    /// The block size: the tile size, or (image width × RowsPerStrip) for strips.
    std::uint32_t blockWidth = 0, blockHeight = 0;
    std::uint32_t blocksAcross = 0, blocksDown = 0;
    std::uint16_t compression = 1;     ///< 1 none, 5 LZW, 8 or 32946 Deflate, 50000 ZSTD
    std::uint16_t predictor = 1;       ///< 1, 2 or 3
    std::uint16_t bitsPerSample = 0;
    std::uint64_t ifdOffset = 0;       ///< the offset of the IFD that is read
    std::uint64_t fileSize = 0;
};

class TiffSource final : public Source {
public:
    /// The default size of the cache of decoded blocks, in blocks.
    static constexpr std::size_t kDefaultCacheBlocks = 64;

    /// Opens a file and reads its header. Throws SourceError.
    static std::unique_ptr<TiffSource> open(const std::string& path,
                                            std::size_t cacheBlocks = kDefaultCacheBlocks);
    ~TiffSource() override;
    TiffSource(const TiffSource&) = delete;
    TiffSource& operator=(const TiffSource&) = delete;

    const SourceInfo& info() const override;
    const TiffLayout& layout() const;

    /// See Source::readWindow. Thread-safe. Throws SourceError.
    void readWindow(std::uint32_t col0, std::uint32_t row0, std::uint32_t width, std::uint32_t height,
                    double* out) const override;

    /// Changes the cache limit (in blocks; 0 = no cache). Thread-safe.
    void setCacheLimit(std::size_t blocks) const;

    struct Impl;

private:
    explicit TiffSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Writes `data` (width · height float32 values, row-major, the first row
/// first) as a GeoTIFF with the georeference `g` and the GDAL_NODATA `noData`.
/// Throws SourceError.
void writeGeoTiff(const std::string& path, std::uint32_t width, std::uint32_t height, const float* data,
                  const Georef& g, std::optional<double> noData);

}  // namespace pyre::tool
