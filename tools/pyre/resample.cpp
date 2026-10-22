#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "commands.hpp"
#include "pyre_source.hpp"
#include "tiff.hpp"

namespace pyre::tool {

namespace {

constexpr std::size_t kCacheBlocks = 16;   // decoded TIFF blocks kept by each open source

bool isRaster(const std::filesystem::path& p) {
    const std::string e = sourceExtension(p.string());
    return e == "tif" || e == "tiff" || e == "pyre";
}

/// The order of the sources: by path; the grids of one pyre file ("f.pyre#K") by K.
bool pathLess(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s, std::string& base, long long& k) {
        const auto hash = s.rfind('#');
        base = s;
        k = -1;
        if (hash != std::string::npos && sourceExtension(s) == "pyre" && hash + 1 < s.size()) {
            base = s.substr(0, hash);
            k = std::atoll(s.c_str() + hash + 1);
        }
    };
    std::string ba, bb;
    long long ka, kb;
    split(a, ba, ka);
    split(b, bb, kb);
    const std::filesystem::path pa(ba), pb(bb);
    if (pa != pb) return pa < pb;
    return ka < kb;
}

std::vector<std::string> expand(const std::vector<std::string>& paths) {
    std::vector<std::filesystem::path> files;
    for (const auto& s : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(s, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(s, ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec))
                if (it->is_regular_file(ec) && isRaster(it->path())) files.push_back(it->path());
            if (ec) throw SourceError(s + ": cannot list the folder: " + ec.message());
        } else {
            files.emplace_back(s);
        }
    }
    // A pyre file with many grids gives one source a grid: "file.pyre#K".
    std::vector<std::string> out;
    for (const auto& f : files) {
        const std::string s = f.string();
        if (sourceExtension(s) == "pyre" && s.find('#') == std::string::npos) {
            const std::size_t n = PyreSource::gridCount(s);
            if (n == 1) out.push_back(s);
            else
                for (std::size_t k = 0; k < n; ++k) out.push_back(s + "#" + std::to_string(k));
        } else {
            out.push_back(s);
        }
    }
    // In order of path: the same order, and so the same choices on a tie, on every run.
    std::sort(out.begin(), out.end(), pathLess);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::unique_ptr<Source> openLimited(const std::string& path) {
    auto s = openSource(path);
    if (auto* t = dynamic_cast<TiffSource*>(s.get())) t->setCacheLimit(kCacheBlocks);
    // A PyreSource keeps its own small cache of decoded tiles.
    return s;
}

}  // namespace

SheetSet::SheetSet(const std::vector<std::string>& paths, std::size_t openLimit) : openLimit_(std::max<std::size_t>(openLimit, 1)) {
    const std::vector<std::string> files = expand(paths);
    if (files.empty()) throw SourceError("no source rasters (.tif, .tiff or .pyre) were found");
    sheets_.resize(files.size());
    parallelFor(files.size(), std::max(1u, std::thread::hardware_concurrency()), [&](std::size_t k) {
        Sheet& s = sheets_[k];
        s.path = files[k];
        s.info = openSource(files[k])->info();
        const Georef& g = s.info.georef;
        if (g.model != ModelType::Geographic)
            throw SourceError(s.path + ": the source is not in a geographic CRS (degrees); pyre-resample needs degrees");
        if (!(g.spacingX > 0) || !(g.spacingY > 0) || s.info.width < 2 || s.info.height < 2)
            throw SourceError(s.path + ": the source must have rows going south and at least 2 × 2 pixels");
        // The north-west corner of pixel (0, 0)'s cell: pixel centres are at
        // lon0 + (i + 0.5) · stepLon, lat0 − (j + 0.5) · stepLat.
        const bool point = g.rasterType == RasterType::PixelIsPoint;
        s.stepLon = g.spacingX;
        s.stepLat = g.spacingY;
        s.lon0 = point ? g.originX - 0.5 * s.stepLon : g.originX;
        s.lat0 = point ? g.originY + 0.5 * s.stepLat : g.originY;
        s.minLon = s.lon0 + 0.5 * s.stepLon;
        s.maxLon = s.lon0 + (double(s.info.width) - 0.5) * s.stepLon;
        s.maxLat = s.lat0 - 0.5 * s.stepLat;
        s.minLat = s.lat0 - (double(s.info.height) - 0.5) * s.stepLat;
    });
    open_.resize(sheets_.size());
}

SheetSet::~SheetSet() = default;

void SheetSet::bounds(double& minLon, double& minLat, double& maxLon, double& maxLat) const {
    minLon = minLat = INFINITY;
    maxLon = maxLat = -INFINITY;
    for (const Sheet& s : sheets_) {
        minLon = std::min(minLon, s.minLon);
        minLat = std::min(minLat, s.minLat);
        maxLon = std::max(maxLon, s.maxLon);
        maxLat = std::max(maxLat, s.maxLat);
    }
}

std::shared_ptr<const Source> SheetSet::open(std::size_t k) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_[k].source) {
            open_[k].used = ++clock_;
            return open_[k].source;
        }
    }
    std::shared_ptr<const Source> s = openLimited(sheets_[k].path);
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_[k].source) {   // another thread was faster
        open_[k].used = ++clock_;
        return open_[k].source;
    }
    open_[k] = {s, ++clock_};
    if (++openCount_ > openLimit_) {
        // Close the least recently used one. A thread that still reads it keeps it alive.
        std::size_t oldest = k;
        for (std::size_t q = 0; q < open_.size(); ++q)
            if (open_[q].source && q != k && (oldest == k || open_[q].used < open_[oldest].used)) oldest = q;
        if (oldest != k) {
            open_[oldest].source.reset();
            --openCount_;
        }
    }
    return s;
}

bool SheetSet::touches(const LocalGrid& g, double x0, double y0, double step, std::uint32_t side) const {
    const double lonA = g.lonOf(x0), lonB = g.lonOf(x0 + double(side - 1) * step);
    const double latA = g.latOf(y0), latB = g.latOf(y0 + double(side - 1) * step);
    for (const Sheet& s : sheets_)
        if (s.maxLon >= lonA && s.minLon <= lonB && s.maxLat >= latA && s.minLat <= latB) return true;
    return false;
}

std::size_t SheetSet::sample(const LocalGrid& g, double x0, double y0, double step, std::uint32_t side,
                             std::vector<double>& out) const {
    const std::size_t n = side;
    out.assign(n * n, NAN);
    // The degrees of each column and each row: lon depends on x alone, lat on y alone.
    std::vector<double> lon(n), lat(n);
    for (std::size_t i = 0; i < n; ++i) lon[i] = g.lonOf(x0 + double(i) * step);
    for (std::size_t j = 0; j < n; ++j) lat[j] = g.latOf(y0 + double(j) * step);

    // The candidates, in the order of the set (of path), and the window of each.
    struct Candidate {
        std::size_t sheet;
        std::vector<double> fx, fy, mx, my;   // pixel coordinates and margins of each column and row
        std::uint32_t cx0, cy0, cols;
        std::vector<double> values;
    };
    std::vector<Candidate> cands;
    for (std::size_t k = 0; k < sheets_.size(); ++k) {
        const Sheet& s = sheets_[k];
        if (s.maxLon < lon.front() || s.minLon > lon.back() || s.maxLat < lat.front() || s.minLat > lat.back()) continue;
        const double w = double(s.info.width), h = double(s.info.height);
        Candidate c;
        c.sheet = k;
        c.fx.resize(n);
        c.mx.resize(n);
        c.fy.resize(n);
        c.my.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            c.fx[i] = (lon[i] - s.lon0) / s.stepLon - 0.5;
            c.mx[i] = std::min(c.fx[i], w - 1 - c.fx[i]);
        }
        for (std::size_t j = 0; j < n; ++j) {
            c.fy[j] = (s.lat0 - lat[j]) / s.stepLat - 0.5;
            c.my[j] = std::min(c.fy[j], h - 1 - c.fy[j]);
        }
        // The pixels that any node can use: columns grow with i, rows fall with j.
        const double ax = std::max(0.0, std::floor(c.fx.front())), bx = std::min(w - 1, std::floor(c.fx.back()) + 1);
        const double ay = std::max(0.0, std::floor(c.fy.back())), by = std::min(h - 1, std::floor(c.fy.front()) + 1);
        if (ax > bx || ay > by) continue;
        c.cx0 = std::uint32_t(ax);
        c.cy0 = std::uint32_t(ay);
        c.cols = std::uint32_t(bx - ax) + 1;
        const std::uint32_t rows = std::uint32_t(by - ay) + 1;
        c.values.resize(std::size_t(c.cols) * rows);
        open(k)->readWindow(c.cx0, c.cy0, c.cols, rows, c.values.data());
        cands.push_back(std::move(c));
    }
    if (cands.empty()) return 0;

    std::size_t count = 0;
    std::vector<std::pair<double, std::size_t>> order;
    order.reserve(cands.size());
    for (std::size_t j = 0; j < n; ++j) {
        double* row = out.data() + j * n;
        for (std::size_t i = 0; i < n; ++i) {
            // The candidates that contain the node, the deepest first; on a tie, by path.
            order.clear();
            for (std::size_t q = 0; q < cands.size(); ++q) {
                const double m = std::min(cands[q].mx[i], cands[q].my[j]);
                if (m < 0) continue;
                auto at = order.end();
                while (at != order.begin() && (at - 1)->first < m) --at;
                order.insert(at, {m, q});
            }
            for (const auto& o : order) {
                const Candidate& c = cands[o.second];
                const SourceInfo& si = sheets_[c.sheet].info;
                const double fx = c.fx[i], fy = c.fy[j];
                const int px = int(std::floor(fx)), py = int(std::floor(fy));
                if (px < 0 || py < 0 || std::uint32_t(px) + 1 >= si.width || std::uint32_t(py) + 1 >= si.height) continue;
                double v[4];
                bool ok = true;
                for (int q = 0; q < 4 && ok; ++q) {
                    const std::uint32_t x = std::uint32_t(px + (q & 1)) - c.cx0, y = std::uint32_t(py + (q >> 1)) - c.cy0;
                    v[q] = c.values[std::size_t(y) * c.cols + x];
                    ok = std::isfinite(v[q]) && !si.isNoData(v[q]) && v[q] >= kLowest;
                }
                if (!ok) continue;
                const double tx = fx - px, ty = fy - py;
                row[i] = double(float((v[0] * (1 - tx) + v[1] * tx) * (1 - ty) + (v[2] * (1 - tx) + v[3] * tx) * ty));
                ++count;
                break;
            }
        }
    }
    return count;
}

}  // namespace pyre::tool
