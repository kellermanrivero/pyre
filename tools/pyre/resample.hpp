#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  resample.hpp — many geographic sources, sampled onto a local metre grid
// ═════════════════════════════════════════════════════════════════════════════
//
// The rule for one point (the same as the reference packs use):
//   1. Take the sources that contain the point: the point is inside the box of
//      their pixel centres.
//   2. Order them by how deep the point is inside: the margin is the distance
//      to the nearest edge of that box, in pixels. The largest margin comes
//      first; on a tie, the source that comes first by path.
//   3. Take the first source whose 4 pixels round the point all have data, and
//      interpolate bilinearly between them (in double, then rounded to float).
//   4. If no source has its 4 pixels, the point has no data.
// The choice depends on the point alone. So a node on the side that two tiles
// share gets the same value from both tiles.
//
// A pixel has no data if the source says so (its no-data value, or NaN), or if
// it is below kLowest (no land is that low; some sources use −9999 and give no
// no-data tag).
//
// Sources must be geographic (degrees). They are opened when a tile needs them
// and closed again when many are open, so a folder of thousands of sheets
// works with a small limit of open files.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "local.hpp"
#include "source.hpp"

namespace pyre::tool {

class SheetSet {
public:
    static constexpr double kLowest = -1000.0;

    /// Reads the headers of `paths` (files, or folders: every .tif, .tiff and
    /// .pyre below them; each grid of a .pyre file is a source). Throws
    /// SourceError if a file cannot be read or is not geographic. `openLimit` is
    /// the most sources kept open at once.
    explicit SheetSet(const std::vector<std::string>& paths, std::size_t openLimit = 64);
    ~SheetSet();

    std::size_t size() const { return sheets_.size(); }
    const SourceInfo& info(std::size_t k) const { return sheets_[k].info; }

    /// The box of all the sources' pixel centres, in degrees.
    void bounds(double& minLon, double& minLat, double& maxLon, double& maxLat) const;

    /// Samples the nodes of a square of `side` × `side` nodes: node (i, j) is at
    /// (x0 + i · step, y0 + j · step) metres on the grid g. `out` gets side²
    /// values, row j major (j = 0 is the south row); NaN where there is no data.
    /// Returns the count of nodes with data. Thread-safe.
    std::size_t sample(const LocalGrid& g, double x0, double y0, double step, std::uint32_t side,
                       std::vector<double>& out) const;

    /// Samples a lattice in degrees: node (i, j) at (lon[i], lat[j]). lon must
    /// rise with i and lat with j (j = 0 is the south row). `out` gets
    /// lon.size() × lat.size() values, row j major; NaN where there is no data.
    /// Returns the count of nodes with data. Thread-safe.
    std::size_t sampleLonLat(const std::vector<double>& lon, const std::vector<double>& lat, std::vector<double>& out) const;

    /// True if a source can have data in the square (a quick test before sample).
    bool touches(const LocalGrid& g, double x0, double y0, double step, std::uint32_t side) const;

private:
    struct Sheet {
        std::string path;
        SourceInfo info;
        double lon0 = 0, lat0 = 0;      ///< the north-west corner of pixel (0, 0)'s cell
        double stepLon = 0, stepLat = 0;
        double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;   ///< the box of the pixel centres
    };
    std::shared_ptr<const Source> open(std::size_t k) const;

    std::vector<Sheet> sheets_;
    std::size_t openLimit_;
    mutable std::mutex mutex_;
    struct Open {
        std::shared_ptr<const Source> source;
        std::uint64_t used = 0;
    };
    mutable std::vector<Open> open_;
    mutable std::uint64_t clock_ = 0;
    mutable std::size_t openCount_ = 0;
};

}  // namespace pyre::tool
