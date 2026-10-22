#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  local.hpp — a local metre grid round a centre point (lat0, lon0)
// ═════════════════════════════════════════════════════════════════════════════
//
// The projection is equirectangular on a sphere of radius 6,371,000 m, with its
// centre at (lat0, lon0):
//
//     x = (lon − lon0) · R · π/180 · cos(lat0)      metres east of the centre
//     y = (lat − lat0) · R · π/180                  metres north of the centre
//
// This is the grid of the reference packs (format 3). To give the same sample
// points as those packs, the arithmetic is theirs, step by step:
//   - lat0 and lon0 are rounded to 1e-7 degrees (integers);
//   - the metres a 1e-7 degree are rounded to float;
//   - x and y are rounded to float before they are turned into degrees.
// So a node is within a few millimetres of the exact projection (the float
// rounding of x and y), and the same node always gives the same degrees.
//
// The grid: tile (ti, tj) has its south-west node at (ti · S · step, tj · S · step)
// metres, and node (i, j) of the tile at (ti · S · step + i · step, tj · …). Tiles can
// be at negative places; the file keeps them in GridInfo::tileI0, tileJ0.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace pyre::tool {

class LocalGrid {
public:
    static constexpr double kEarthRadius = 6371000.0;
    static constexpr double kNano = 1e-7;
    static constexpr double kPi = 3.14159265358979323846;

    LocalGrid() = default;
    /// The centre in degrees. It is kept to 1e-7 degrees.
    LocalGrid(double lat0, double lon0)
        : lat0e7_(std::int32_t(std::llround(lat0 / kNano))), lon0e7_(std::int32_t(std::llround(lon0 / kNano))) {
        const double lat0Rad = lat0e7_ * kNano * kPi / 180.0;
        const double perNanoLat = kEarthRadius * (kPi / 180.0) * kNano;
        const double perNanoLon = perNanoLat * std::cos(lat0Rad);
        mLat_ = static_cast<float>(perNanoLat);
        mLon_ = static_cast<float>(perNanoLon);
    }

    double lat0() const { return lat0e7_ * kNano; }
    double lon0() const { return lon0e7_ * kNano; }
    std::int32_t lat0e7() const { return lat0e7_; }
    std::int32_t lon0e7() const { return lon0e7_; }

    /// Degrees of a point x metres east, y metres north of the centre.
    double latOf(double y) const { return (double(lat0e7_) + double(float(y)) / double(mLat_)) * kNano; }
    double lonOf(double x) const { return (double(lon0e7_) + double(float(x)) / double(mLon_)) * kNano; }
    /// Metres of a point in degrees (the inverse, to choose tiles; not exact to the float).
    double xOf(double lon) const { return (lon / kNano - double(lon0e7_)) * double(mLon_); }
    double yOf(double lat) const { return (lat / kNano - double(lat0e7_)) * double(mLat_); }

    /// The CRS as WKT 1. The false northing moves y = 0 from the equator to lat0.
    std::string wkt() const {
        char b[640];
        std::snprintf(b, sizeof b,
                      "PROJCS[\"pyre local %.7f %.7f\",GEOGCS[\"Sphere\",DATUM[\"Sphere\",SPHEROID[\"Sphere\",6371000,0]],"
                      "PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Equirectangular\"],"
                      "PARAMETER[\"standard_parallel_1\",%.7f],PARAMETER[\"central_meridian\",%.7f],"
                      "PARAMETER[\"false_easting\",0],PARAMETER[\"false_northing\",%.6f],UNIT[\"metre\",1]]",
                      lat0(), lon0(), lat0(), lon0(), -kEarthRadius * lat0() * kPi / 180.0);
        return b;
    }

    /// The centre of a grid that wkt() wrote; empty for any other CRS.
    static std::optional<LocalGrid> fromWkt(const std::string& wkt) {
        double lat = 0, lon = 0;
        if (wkt.rfind("PROJCS[\"pyre local ", 0) != 0 || std::sscanf(wkt.c_str(), "PROJCS[\"pyre local %lf %lf\"", &lat, &lon) != 2)
            return std::nullopt;
        return LocalGrid(lat, lon);
    }

    bool operator==(const LocalGrid& o) const { return lat0e7_ == o.lat0e7_ && lon0e7_ == o.lon0e7_; }

private:
    std::int32_t lat0e7_ = 0, lon0e7_ = 0;
    float mLat_ = 0, mLon_ = 0;
};

}  // namespace pyre::tool
