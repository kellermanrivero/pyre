#include "pyre_source.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "files.hpp"
#include "pyre/container.hpp"

namespace pyre::tool {

namespace {

struct Decoded {
    std::vector<std::int32_t> heights;
    std::vector<std::uint8_t> mask;   // empty: all valid
};

std::int64_t floorDiv(std::int64_t x, std::int64_t y) { return x >= 0 ? x / y : -((-x + y - 1) / y); }

/// Splits "file.pyre#K" into the file and K (0 and false if there is no "#K").
std::string splitPath(const std::string& path, std::uint32_t& grid, bool& given) {
    grid = 0;
    given = false;
    const auto hash = path.rfind('#');
    if (hash == std::string::npos || hash + 1 == path.size()) return path;
    std::uint64_t k = 0;
    for (std::size_t q = hash + 1; q < path.size(); ++q) {
        if (path[q] < '0' || path[q] > '9' || k > 0xFFFFFFFFu) return path;
        k = k * 10 + std::uint64_t(path[q] - '0');
    }
    grid = std::uint32_t(k);
    given = true;
    return path.substr(0, hash);
}

}  // namespace

struct PyreSource::Impl {
    SourceInfo info;
    std::unique_ptr<MappedFile> map;
    Reader reader;
    std::uint32_t grid = 0;
    GridInfo g;
    Params p;
    double unit = 0.01;
    std::int64_t firstI = 0, firstJ = 0;   // the grid's first node

    std::size_t cacheLimit = kDefaultCacheTiles;
    mutable std::mutex mutex;
    mutable std::map<std::size_t, std::pair<std::shared_ptr<const Decoded>, std::uint64_t>> cache;
    mutable std::uint64_t clock = 0;

    std::shared_ptr<const Decoded> tile(std::size_t k) const {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (auto it = cache.find(k); it != cache.end()) {
                it->second.second = ++clock;
                return it->second.first;
            }
        }
        if (!reader.verifyTile(k)) throw SourceError(info.path + ": a tile fails its checksum; the file is damaged");
        auto d = std::make_shared<Decoded>();
        d->heights.resize(p.nodes());
        if (reader.entry(k).flags & kTileHoles) d->mask.resize(p.nodes());
        Scratch s;
        if (reader.decodeWindow(k, 0, 0, p.tile, p.tile, 1, d->heights, d->mask, s) != Status::Ok)
            throw SourceError(info.path + ": a tile cannot be decoded; the file is damaged");
        std::lock_guard<std::mutex> lock(mutex);
        cache[k] = {d, ++clock};
        while (cache.size() > std::max<std::size_t>(cacheLimit, 1)) {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->second.second < oldest->second.second) oldest = it;
            cache.erase(oldest);
        }
        return d;
    }

    /// The tile of the grid at (ti, tj), if it is in the file.
    std::shared_ptr<const Decoded> tileAt(std::int64_t ti, std::int64_t tj) const {
        if (ti < g.tileI0 || tj < g.tileJ0 || ti >= std::int64_t(g.tileI0) + g.tilesX || tj >= std::int64_t(g.tileJ0) + g.tilesY)
            return nullptr;
        const auto k = reader.find(grid, std::int32_t(ti), std::int32_t(tj));
        return k ? tile(*k) : nullptr;
    }
};

std::unique_ptr<PyreSource> PyreSource::open(const std::string& path, std::size_t cacheTiles) {
    auto m = std::make_unique<Impl>();
    bool given = false;
    const std::string file = splitPath(path, m->grid, given);
    m->map = std::make_unique<MappedFile>(file);
    if (const Status st = m->reader.open(m->map->bytes()); st != Status::Ok)
        throw SourceError(file + ": " + toString(st) + ": " + m->reader.error());
    const FileInfo& fi = m->reader.info();
    if (!given && fi.grids.size() != 1)
        throw SourceError(file + " has " + std::to_string(fi.grids.size()) + " grids; name one as " + file + "#K");
    if (m->grid >= fi.grids.size()) throw SourceError(file + ": there is no grid " + std::to_string(m->grid));
    m->g = fi.grids[m->grid];
    m->p = fi.params;
    m->unit = fi.unit;
    m->cacheLimit = cacheTiles;
    const GridInfo& g = m->g;
    if (g.width < 1 || g.height < 1) throw SourceError(path + ": the grid is empty");
    m->firstI = std::int64_t(g.tileI0) * m->p.tile;
    m->firstJ = std::int64_t(g.tileJ0) * m->p.tile;

    SourceInfo& si = m->info;
    si.path = path;
    si.width = g.width;
    si.height = g.height;
    si.sample = SampleType::F32;
    Georef& r = si.georef;
    r.model = ModelType(g.modelType);
    if (g.epsg) r.epsg = g.epsg;
    r.wkt = g.wkt;
    if (g.verticalEpsg) r.verticalEpsg = g.verticalEpsg;
    r.verticalUnit = g.verticalUnit;
    r.rasterType = g.rasterType == 1 ? RasterType::PixelIsArea : RasterType::PixelIsPoint;
    r.spacingX = g.spacingX;
    r.spacingY = g.spacingY;
    // Pixel (0, 0) is the north-west node: the first column, the last row of nodes.
    r.originX = g.originX + double(m->firstI) * g.spacingX;
    r.originY = g.originY + double(m->firstJ + std::int64_t(g.height) - 1) * g.spacingY;
    if (std::isfinite(fi.noData)) si.noData = fi.noData;
    return std::unique_ptr<PyreSource>(new PyreSource(std::move(m)));
}

std::size_t PyreSource::gridCount(const std::string& file) {
    MappedFile map(file);
    Reader r;
    if (const Status st = r.open(map.bytes()); st != Status::Ok) throw SourceError(file + ": " + toString(st) + ": " + r.error());
    return r.info().grids.size();
}

PyreSource::PyreSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PyreSource::~PyreSource() = default;

const SourceInfo& PyreSource::info() const { return impl_->info; }

void PyreSource::readWindow(std::uint32_t col0, std::uint32_t row0, std::uint32_t width, std::uint32_t height, double* out) const {
    const Impl& m = *impl_;
    if (std::uint64_t(col0) + width > m.info.width || std::uint64_t(row0) + height > m.info.height)
        throw SourceError(m.info.path + ": the window is outside the raster");
    if (width == 0 || height == 0) return;
    const std::int64_t S = m.p.tile, side = std::int64_t(m.p.side());
    // The nodes of the window: I from i0 to i1, J from j0 (south) to j1 (north).
    const std::int64_t i0 = m.firstI + col0, i1 = i0 + width - 1;
    const std::int64_t j1 = m.firstJ + std::int64_t(m.info.height) - 1 - row0, j0 = j1 - (height - 1);
    std::fill(out, out + std::size_t(width) * height, NAN);
    auto put = [&](std::int64_t I, std::int64_t J, double v) { out[std::size_t(j1 - J) * width + std::size_t(I - i0)] = v; };
    // A node's value in tile (ti, tj), if that tile is in the file and has the node.
    auto valueIn = [&](const Decoded* d, std::int64_t ti, std::int64_t tj, std::int64_t I, std::int64_t J, double& v) {
        if (!d) return false;
        const std::int64_t i = I - ti * S, j = J - tj * S;
        if (i < 0 || j < 0 || i > S || j > S) return false;
        const std::size_t t = std::size_t(j * side + i);
        if (!d->mask.empty() && !d->mask[t]) return false;
        // As a float32 raster holds it: for a float32 source with whole quanta
        // (the MDT02 has whole mm) this is the source's value, bit for bit.
        v = double(float(double(d->heights[t]) * m.unit));
        return true;
    };
    // Each tile gives the nodes it owns: I from ti·S to ti·S + S − 1 (and J likewise).
    // A node on a tile's west or south side is also in the neighbour tile: it
    // is read from there when the owner is not in the file.
    for (std::int64_t tj = floorDiv(j0, S); tj <= floorDiv(j1, S); ++tj)
        for (std::int64_t ti = floorDiv(i0, S); ti <= floorDiv(i1, S); ++ti) {
            const auto own = m.tileAt(ti, tj);
            std::shared_ptr<const Decoded> west, south, southWest;
            bool lookedWest = false, lookedSouth = false, lookedSouthWest = false;
            for (std::int64_t J = std::max(j0, tj * S); J <= std::min(j1, tj * S + S - 1); ++J)
                for (std::int64_t I = std::max(i0, ti * S); I <= std::min(i1, ti * S + S - 1); ++I) {
                    double v;
                    if (valueIn(own.get(), ti, tj, I, J, v)) {
                        put(I, J, v);
                        continue;
                    }
                    if (own && !(I == ti * S || J == tj * S)) continue;   // a hole inside the tile
                    if (I == ti * S) {
                        if (!lookedWest) west = m.tileAt(ti - 1, tj), lookedWest = true;
                        if (valueIn(west.get(), ti - 1, tj, I, J, v)) {
                            put(I, J, v);
                            continue;
                        }
                    }
                    if (J == tj * S) {
                        if (!lookedSouth) south = m.tileAt(ti, tj - 1), lookedSouth = true;
                        if (valueIn(south.get(), ti, tj - 1, I, J, v)) {
                            put(I, J, v);
                            continue;
                        }
                    }
                    if (I == ti * S && J == tj * S) {
                        if (!lookedSouthWest) southWest = m.tileAt(ti - 1, tj - 1), lookedSouthWest = true;
                        if (valueIn(southWest.get(), ti - 1, tj - 1, I, J, v)) put(I, J, v);
                    }
                }
        }
}

}  // namespace pyre::tool
