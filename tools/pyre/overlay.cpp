#include "overlay.hpp"

#include <algorithm>
#include <cmath>

namespace pyre::tool {

namespace {

/// x / step as a whole number, if it is one (to 1e-6 of a step).
bool wholeSteps(double x, double step, std::int64_t& n) {
    const double q = x / step;
    if (!std::isfinite(q) || std::abs(q - std::round(q)) > 1e-6 || std::abs(q) > 9e15) return false;
    n = std::int64_t(std::llround(q));
    return true;
}

}  // namespace

Overlay::Overlay(const std::vector<std::string>& paths, const LocalGrid& g, double step) {
    for (const auto& path : paths) {
        Layer l;
        l.source = openSource(path);
        const SourceInfo& si = l.source->info();
        const Georef& r = si.georef;
        if (const auto lg = LocalGrid::fromWkt(r.wkt)) {
            if (!(*lg == g)) throw SourceError(path + ": the overlay is on another local grid (another centre)");
        } else if (r.epsg || !r.wkt.empty()) {
            throw SourceError(path + ": the overlay is not on this local grid (its CRS is " +
                              (r.epsg ? "EPSG:" + std::to_string(*r.epsg) : r.wkt.substr(0, 60)) + ")");
        }
        if (std::abs(r.spacingX - step) > 1e-9 * step || std::abs(r.spacingY - step) > 1e-9 * step)
            throw SourceError(path + ": the overlay's pixel is not the grid step of " + std::to_string(step) + " m");
        // The sample point of pixel (0, 0).
        double x = r.originX, y = r.originY;
        if (r.rasterType == RasterType::PixelIsArea) {
            x += 0.5 * r.spacingX;
            y -= 0.5 * r.spacingY;
        }
        if (!wholeSteps(x, step, l.firstI) || !wholeSteps(y, step, l.topJ))
            throw SourceError(path + ": the overlay's pixels are not on the grid's nodes");
        l.width = si.width;
        l.height = si.height;
        layers_.push_back(std::move(l));
    }
}

Overlay::~Overlay() = default;

std::size_t Overlay::apply(std::int64_t I0, std::int64_t J0, std::uint32_t side, std::vector<double>& values) const {
    std::vector<bool> set(std::size_t(side) * side, false);
    std::vector<double> buf;
    const std::int64_t I1 = I0 + side - 1, J1 = J0 + side - 1;
    for (const Layer& l : layers_) {
        // The overlay's nodes: I from firstI to firstI + width − 1, J from topJ − height + 1 to topJ.
        const std::int64_t a0 = std::max(I0, l.firstI), a1 = std::min(I1, l.firstI + std::int64_t(l.width) - 1);
        const std::int64_t b0 = std::max(J0, l.topJ - std::int64_t(l.height) + 1), b1 = std::min(J1, l.topJ);
        if (a0 > a1 || b0 > b1) continue;
        const std::uint32_t cols = std::uint32_t(a1 - a0 + 1), rows = std::uint32_t(b1 - b0 + 1);
        buf.resize(std::size_t(cols) * rows);
        // Row 0 of the window is the northmost: J = b1.
        l.source->readWindow(std::uint32_t(a0 - l.firstI), std::uint32_t(l.topJ - b1), cols, rows, buf.data());
        const SourceInfo& si = l.source->info();
        for (std::uint32_t r = 0; r < rows; ++r)
            for (std::uint32_t c = 0; c < cols; ++c) {
                const double v = buf[std::size_t(r) * cols + c];
                if (!std::isfinite(v) || si.isNoData(v)) continue;
                const std::size_t t = std::size_t(b1 - r - J0) * side + std::size_t(a0 + c - I0);
                values[t] = v;
                set[t] = true;
            }
    }
    return std::size_t(std::count(set.begin(), set.end(), true));
}

}  // namespace pyre::tool
