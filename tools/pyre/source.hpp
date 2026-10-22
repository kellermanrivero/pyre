#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  source.hpp — the source interface: one reader for each format
// ═════════════════════════════════════════════════════════════════════════════
//
// A reader gives the metadata of one raster and reads windows of its values.
// It does not cut tiles and it does not resample: that is the grid layer's job.
// Keep the two apart, so that a later resampler can sit between them.
//
// Conventions:
// - Pixel (col 0, row 0) is the first pixel of the file (normally the
//   north-west one). Rows go south when Georef::spacingY is positive.
// - Values are double: that holds int16, int32, float32 and float64 exactly.
//   A no-data pixel keeps its raw value; SourceInfo::isNoData says which.
// - The tool may use exceptions (it is not built into the game): a reader
//   throws SourceError.

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace pyre::tool {

struct SourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// GeoTIFF's raster type: what a pixel's value stands for.
enum class RasterType : std::uint8_t {
    PixelIsArea = 1,    ///< the mean of the cell; the tie point is the cell's corner
    PixelIsPoint = 2,   ///< a point sample; the tie point is the sample itself
};

enum class ModelType : std::uint8_t { Unknown = 0, Projected = 1, Geographic = 2, Geocentric = 3 };

struct Georef {
    ModelType model = ModelType::Unknown;
    std::optional<std::uint32_t> epsg;            ///< the horizontal CRS, if it has a code
    std::string wkt;                              ///< WKT or a citation, when there is no code
    std::optional<std::uint32_t> verticalEpsg;    ///< the vertical CRS, if given
    std::string verticalUnit;                     ///< "metre", if given
    RasterType rasterType = RasterType::PixelIsArea;
    /// The CRS coordinate of pixel (0, 0)'s reference point: its north-west
    /// corner for PixelIsArea, the sample itself for PixelIsPoint.
    double originX = 0, originY = 0;
    /// The pixel size in CRS units: spacingX > 0 (columns go east); spacingY > 0
    /// when rows go south (the normal case).
    double spacingX = 1, spacingY = 1;
};

enum class SampleType : std::uint8_t { U8, I16, U16, I32, U32, F32, F64 };

struct SourceInfo {
    std::string path;
    std::uint32_t width = 0, height = 0;
    SampleType sample = SampleType::F32;
    Georef georef;
    std::optional<double> noData;   ///< GDAL_NODATA, if given

    bool isNoData(double v) const { return std::isnan(v) || (noData && v == *noData); }
};

/// One source raster. readWindow is const and must be safe to call from many
/// threads at once.
class Source {
public:
    virtual ~Source() = default;
    virtual const SourceInfo& info() const = 0;
    /// Reads columns [col0, col0 + width) and rows [row0, row0 + height) into
    /// `out` (row-major, width · height values, the first row first). The
    /// window must be inside the raster. Throws SourceError.
    virtual void readWindow(std::uint32_t col0, std::uint32_t row0, std::uint32_t width, std::uint32_t height,
                            double* out) const = 0;
};

/// Opens a source by its file name (source.cpp); add each new reader there.
/// "file.pyre#K" opens grid K of a pyre file. Throws SourceError.
std::unique_ptr<Source> openSource(const std::string& path);

/// The extension of a source path, in lower case, without a "#K" suffix.
std::string sourceExtension(const std::string& path);

}  // namespace pyre::tool
