#include "grid.hpp"

#include <algorithm>

namespace pyre::tool {

std::string baseName(const std::string& path) {
    const auto k = path.find_last_of('/');
    return k == std::string::npos ? path : path.substr(k + 1);
}

GridInfo gridOf(const SourceInfo& s, const Params& p) {
    if (s.width < 2 || s.height < 2) throw SourceError(s.path + ": a raster needs at least 2 × 2 pixels");
    GridInfo g;
    const Georef& r = s.georef;
    g.epsg = r.epsg.value_or(0);
    g.verticalEpsg = r.verticalEpsg.value_or(0);
    g.rasterType = std::uint8_t(r.rasterType);
    g.modelType = std::uint8_t(r.model);
    g.width = s.width;
    g.height = s.height;
    g.tilesX = (s.width - 2) / p.tile + 1;    // ⌈(W − 1)/S⌉
    g.tilesY = (s.height - 2) / p.tile + 1;
    // Node (0, 0) is the south-west pixel's reference point.
    g.originX = r.originX;
    g.originY = r.originY - double(s.height - 1) * r.spacingY;
    g.spacingX = r.spacingX;
    g.spacingY = r.spacingY;
    g.wkt = r.epsg ? std::string() : r.wkt;
    g.verticalUnit = r.verticalUnit;
    g.name = baseName(s.path);
    return g;
}

std::int32_t toQuanta(double v, double unit) {
    const double q = std::round(v / unit);
    if (!(q >= -double(kMaxHeight) && q <= double(kMaxHeight)))
        throw SourceError("height " + std::to_string(v) + " is out of range for the unit " + std::to_string(unit));
    return std::int32_t(q);
}

std::int32_t errorQuanta(double epsilon, double unit) {
    if (!(epsilon >= 0)) throw SourceError("epsilon must be 0 or more");
    const double d = std::floor(epsilon / unit + 1e-9);
    if (d > double(kMaxError)) throw SourceError("epsilon is too large for the unit");
    return std::int32_t(d);
}

namespace {
struct Window {
    std::uint32_t i0, j0, i1, j1;   // tile nodes inside the raster, inclusive
    bool empty;
};

Window windowOf(const SourceInfo& s, const Params& p, std::uint32_t ti, std::uint32_t tj) {
    const std::uint64_t I0 = std::uint64_t(ti) * p.tile, J0 = std::uint64_t(tj) * p.tile;
    if (I0 >= s.width || J0 >= s.height) return {0, 0, 0, 0, true};
    const std::uint64_t I1 = std::min<std::uint64_t>(I0 + p.tile, s.width - 1), J1 = std::min<std::uint64_t>(J0 + p.tile, s.height - 1);
    return {0, 0, std::uint32_t(I1 - I0), std::uint32_t(J1 - J0), false};
}

/// Reads the raster under the window into buf: row r of buf is tile row j1 − r.
void readWindowOf(const Source& src, const Params& p, std::uint32_t ti, std::uint32_t tj, const Window& w, std::vector<double>& buf) {
    const SourceInfo& s = src.info();
    const std::uint32_t I0 = ti * p.tile, J0 = tj * p.tile;
    const std::uint32_t cols = w.i1 + 1, rows = w.j1 + 1;
    buf.resize(std::size_t(cols) * rows);
    // Tile row j is raster row H − 1 − (J0 + j); the window's first row is the northmost.
    src.readWindow(I0, s.height - 1 - (J0 + w.j1), cols, rows, buf.data());
}
}  // namespace

std::size_t readTile(const Source& src, const Params& p, double unit, std::uint32_t ti, std::uint32_t tj,
                     std::vector<std::int32_t>& x, std::vector<std::uint8_t>& valid, std::vector<double>& buf) {
    const SourceInfo& s = src.info();
    const std::size_t side = p.side();
    x.assign(p.nodes(), 0);
    valid.assign(p.nodes(), 0);
    const Window w = windowOf(s, p, ti, tj);
    if (w.empty) return 0;
    readWindowOf(src, p, ti, tj, w, buf);
    const std::size_t cols = w.i1 + 1;
    std::size_t count = 0;
    for (std::uint32_t j = 0; j <= w.j1; ++j) {
        const double* row = buf.data() + std::size_t(w.j1 - j) * cols;
        std::int32_t* xr = x.data() + j * side;
        std::uint8_t* vr = valid.data() + j * side;
        for (std::uint32_t i = 0; i <= w.i1; ++i) {
            const double v = row[i];
            if (s.isNoData(v)) continue;
            xr[i] = toQuanta(v, unit);
            vr[i] = 1;
            ++count;
        }
    }
    return count;
}

void readTileValues(const Source& src, const Params& p, std::uint32_t ti, std::uint32_t tj, std::vector<double>& v) {
    const SourceInfo& s = src.info();
    const std::size_t side = p.side();
    v.assign(p.nodes(), NAN);
    const Window w = windowOf(s, p, ti, tj);
    if (w.empty) return;
    std::vector<double> buf;
    readWindowOf(src, p, ti, tj, w, buf);
    const std::size_t cols = w.i1 + 1;
    for (std::uint32_t j = 0; j <= w.j1; ++j)
        for (std::uint32_t i = 0; i <= w.i1; ++i) {
            const double x = buf[std::size_t(w.j1 - j) * cols + i];
            v[j * side + i] = s.isNoData(x) ? NAN : x;
        }
}

bool tileTouches(const GridInfo& g, const Params& p, std::int32_t ti, std::int32_t tj, const BBox& b) {
    if (b.all()) return true;
    const double x0 = g.originX + double(ti) * p.tile * g.spacingX, x1 = x0 + p.tile * g.spacingX;
    const double y0 = g.originY + double(tj) * p.tile * g.spacingY, y1 = y0 + p.tile * g.spacingY;
    return std::max(x0, b.minX) <= std::min(x1, b.maxX) && std::max(y0, b.minY) <= std::min(y1, b.maxY);
}

}  // namespace pyre::tool
