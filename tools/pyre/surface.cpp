#include "surface.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pyre::tool {

std::int32_t surfaceSteps(double h, const SurfaceOptions& o) {
    if (!(h >= o.min)) return 0;
    const double k = std::round(h / o.unit);
    return std::int32_t(std::clamp(k, 1.0, double(kMaxHeight)));
}

NodePlaces::NodePlaces(const GridInfo& g) : g_(g), local_(LocalGrid::fromWkt(g.wkt)) {
    if (!local_ && g.modelType != std::uint8_t(ModelType::Geographic))
        throw std::runtime_error("grid " + g.name + " is neither in degrees nor a pyre-resample local grid");
    area_ = g.rasterType == std::uint8_t(RasterType::PixelIsArea);
}

double NodePlaces::lon(std::int64_t I) const {
    if (local_) return local_->lonOf(g_.originX + double(I) * g_.spacingX);
    // For PixelIsArea, a node's place is the north-west corner of its cell: the sample is at its centre.
    return g_.originX + double(I) * g_.spacingX + (area_ ? 0.5 * g_.spacingX : 0.0);
}

double NodePlaces::lat(std::int64_t J) const {
    if (local_) return local_->latOf(g_.originY + double(J) * g_.spacingY);
    return g_.originY + double(J) * g_.spacingY - (area_ ? 0.5 * g_.spacingY : 0.0);
}

std::size_t SurfaceMaker::make(const Source& ground, const GridInfo& g, const NodePlaces& places, const Params& p, std::int32_t ti,
                               std::int32_t tj, std::vector<std::int32_t>& x, std::vector<std::uint8_t>& valid) const {
    const std::int64_t r = o_.stride, half = r / 2, S = p.tile;
    const std::int64_t side = S / r + 1;
    x.assign(std::size_t(side * side), 0);
    valid.assign(x.size(), 0);

    // The grid nodes that the layer tile needs: I from I0 to I1, J from J0 to J1.
    const std::int64_t I0 = std::int64_t(ti) * S - half, I1 = std::int64_t(ti) * S + S + (r - half - 1);
    const std::int64_t J0 = std::int64_t(tj) * S - half, J1 = std::int64_t(tj) * S + S + (r - half - 1);
    const std::int64_t nx = I1 - I0 + 1, ny = J1 - J0 + 1;

    // The ground (NaN where there is none). Node (I, J) is pixel (I − firstI, topJ − J).
    const SourceInfo& si = ground.info();
    const std::int64_t firstI = std::int64_t(g.tileI0) * S, topJ = std::int64_t(g.tileJ0) * S + std::int64_t(si.height) - 1;
    std::vector<double> base(std::size_t(nx * ny), NAN);
    const std::int64_t c0 = std::max(I0, firstI), c1 = std::min(I1, firstI + std::int64_t(si.width) - 1);
    const std::int64_t j0 = std::max(J0, topJ - std::int64_t(si.height) + 1), j1 = std::min(J1, topJ);
    if (c0 > c1 || j0 > j1) return 0;
    {
        const std::uint32_t cols = std::uint32_t(c1 - c0 + 1), rows = std::uint32_t(j1 - j0 + 1);
        std::vector<double> buf(std::size_t(cols) * rows);
        ground.readWindow(std::uint32_t(c0 - firstI), std::uint32_t(topJ - j1), cols, rows, buf.data());
        for (std::int64_t J = j0; J <= j1; ++J)
            for (std::int64_t I = c0; I <= c1; ++I) {
                const double v = buf[std::size_t(j1 - J) * cols + std::size_t(I - c0)];
                if (std::isfinite(v) && !si.isNoData(v)) base[std::size_t((J - J0) * nx + (I - I0))] = v;
            }
    }

    // The surface at the same nodes.
    std::vector<double> lon(static_cast<std::size_t>(nx)), lat(static_cast<std::size_t>(ny));
    for (std::int64_t I = I0; I <= I1; ++I) lon[std::size_t(I - I0)] = places.lon(I);
    for (std::int64_t J = J0; J <= J1; ++J) lat[std::size_t(J - J0)] = places.lat(J);
    std::vector<double> top;
    surface_.sampleLonLat(lon, lat, top);

    // Each layer node: the largest h round it, in steps.
    std::size_t count = 0;
    for (std::int64_t jj = 0; jj < side; ++jj)
        for (std::int64_t ii = 0; ii < side; ++ii) {
            double best = -INFINITY;
            bool any = false;
            for (std::int64_t b = -half; b < r - half; ++b)
                for (std::int64_t a = -half; a < r - half; ++a) {
                    const std::int64_t I = std::int64_t(ti) * S + r * ii + a, J = std::int64_t(tj) * S + r * jj + b;
                    const std::size_t t = std::size_t((J - J0) * nx + (I - I0));
                    if (std::isnan(base[t]) || std::isnan(top[t])) continue;
                    best = std::max(best, top[t] - base[t]);
                    any = true;
                }
            if (!any) continue;
            const std::size_t q = std::size_t(jj * side + ii);
            x[q] = surfaceSteps(best, o_);
            valid[q] = 1;
            ++count;
        }
    return count;
}

}  // namespace pyre::tool
