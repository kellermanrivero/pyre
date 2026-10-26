// The commands info, encode, decode and verify. (bench is in bench.cpp.)

#include "commands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

#include "files.hpp"
#include "grid.hpp"
#include "json.hpp"
#include "tiff.hpp"

namespace pyre::tool {

// ── Helpers ──────────────────────────────────────────────────────────────────

bool takesValue(const std::string& o) {
    static const std::set<std::string> v = {"output", "epsilon", "unit", "tile", "block", "base", "block-levels",
                                            "bbox", "threads", "level", "grid", "nodata", "max-tiles", "layer",
                                            "layer-of", "kind", "min", "stride"};
    return v.count(o) != 0;
}

Args parseArgs(int argc, char** argv, int first, const std::function<bool(const std::string&)>& takes) {
    Args a;
    for (int k = first; k < argc; ++k) {
        std::string s = argv[k];
        if (s == "-o") s = "--output";
        if (s.size() > 2 && s.compare(0, 2, "--") == 0) {
            const std::string name = s.substr(2);
            if (takes(name)) {
                if (k + 1 >= argc) throw std::runtime_error("--" + name + " needs a value");
                a.options[name] = argv[++k];
                a.all[name].push_back(argv[k]);
            } else {
                a.options[name] = "1";
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

double Args::number(const std::string& k, double def) const {
    if (!has(k)) return def;
    const std::string s = get(k);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != 0) throw std::runtime_error("--" + k + " needs a number, not '" + s + "'");
    return v;
}

std::uint32_t Args::count(const std::string& k, std::uint32_t def) const {
    const double v = number(k, def);
    if (v < 0 || v != std::floor(v) || v > 4294967295.0) throw std::runtime_error("--" + k + " needs a whole number");
    return static_cast<std::uint32_t>(v);
}

unsigned threadsOf(const Args& a) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::max(1u, a.count("threads", hw));
}

void parallelFor(std::size_t n, unsigned threads, const std::function<void(std::size_t)>& f) {
    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex m;
    auto work = [&] {
        for (;;) {
            const std::size_t k = next.fetch_add(1);
            if (k >= n) return;
            try {
                f(k);
            } catch (...) {
                std::lock_guard<std::mutex> lock(m);
                if (!error) error = std::current_exception();
                next = n;
            }
        }
    };
    threads = static_cast<unsigned>(std::min<std::size_t>(threads, std::max<std::size_t>(n, 1)));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

Params paramsOf(const Args& a) {
    Params p;
    p.tile = a.count("tile", 1000);
    p.block = a.count("block", 200);
    p.base = a.count("base", 8);
    p.blockLevels = a.count("block-levels", 1);
    if (!p.valid())
        throw std::runtime_error("bad parameters: tile " + std::to_string(p.tile) + ", block " + std::to_string(p.block) +
                                 ", base " + std::to_string(p.base) + ", block levels " + std::to_string(p.blockLevels) +
                                 " (base: a power of 2 up to 256; block levels: 1 to log2(base); tile: a multiple of"
                                 " base and of block; block: a multiple of 2^block-levels)");
    return p;
}

BBox bboxOf(const Args& a, const std::string& name) {
    BBox b;
    if (!a.has(name)) return b;
    double v[4];
    if (std::sscanf(a.get(name).c_str(), "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]) != 4 || v[0] > v[2] || v[1] > v[3])
        throw std::runtime_error("--" + name + " needs minX,minY,maxX,maxY");
    b.minX = v[0];
    b.minY = v[1];
    b.maxX = v[2];
    b.maxY = v[3];
    return b;
}

void need(Status s, const char* what) {
    if (s != Status::Ok) throw std::runtime_error(std::string(what) + ": " + toString(s));
}

namespace {

std::string crsText(const GridInfo& g) {
    if (g.epsg) return "EPSG:" + std::to_string(g.epsg);
    if (!g.wkt.empty()) return g.wkt;
    return "unknown";
}

const char* rasterTypeText(std::uint8_t t) { return t == 1 ? "PixelIsArea" : t == 2 ? "PixelIsPoint" : "?"; }

const char* kindName(std::uint8_t k) { return k == kKindAboveGround ? "height above ground" : "unknown"; }

struct OpenFile {
    std::unique_ptr<MappedFile> map;
    Reader reader;
    explicit OpenFile(const std::string& path) : map(std::make_unique<MappedFile>(path)) {
        const Status st = reader.open(map->bytes());
        if (st != Status::Ok) throw std::runtime_error(path + ": " + toString(st) + ": " + reader.error());
    }
};

}  // namespace

// ── info ─────────────────────────────────────────────────────────────────────

int cmdInfo(const Args& a) {
    if (a.positional.size() != 1) throw std::runtime_error("usage: pyre info <file.pyre>");
    OpenFile f(a.positional[0]);
    const Reader& r = f.reader;
    const FileInfo& fi = r.info();
    const Params& p = fi.params;
    std::uint64_t tileBytes = 0, headBytes = 0, holes = 0;
    std::int32_t lo = INT32_MAX, hi = INT32_MIN;
    std::set<std::int32_t> epsilons;
    for (std::size_t k = 0; k < r.tiles(); ++k) {
        const TileEntry e = r.entry(k);
        tileBytes += e.totalBytes;
        headBytes += e.headBytes;
        holes += (e.flags & kTileHoles) != 0;
        lo = std::min(lo, e.min);
        hi = std::max(hi, e.max);
        if (e.d != fi.d) epsilons.insert(e.d);
    }
    const std::uint64_t fileBytes = f.map->bytes().size();
    // Each tile holds S² nodes of its own (its shared edges are counted once).
    const double nodes = static_cast<double>(r.tiles()) * static_cast<double>(p.tile) * static_cast<double>(p.tile);
    const double bitsPerNode = nodes > 0 ? static_cast<double>(fileBytes) * 8 / nodes : 0;

    if (a.has("json")) {
        Json j;
        j.add("file", a.positional[0]).add("version", static_cast<unsigned>(kVersion)).add("bytes", static_cast<std::uint64_t>(fileBytes));
        j.add("tile", static_cast<unsigned>(p.tile)).add("block", static_cast<unsigned>(p.block)).add("base", static_cast<unsigned>(p.base)).add("block_levels", static_cast<unsigned>(p.blockLevels));
        j.add("method", static_cast<unsigned>(fi.method.bits())).add("unit", fi.unit).add("epsilon", fi.d * fi.unit).add("error_quanta", fi.d);
        j.add("nodata", fi.noData).add("tiles", static_cast<std::uint64_t>(r.tiles())).add("tiles_with_holes", holes);
        j.add("header_bytes", static_cast<std::uint64_t>(r.headerBytes())).add("index_bytes", r.indexBytes()).add("tile_bytes", tileBytes);
        j.add("head_bytes", headBytes).add("bits_per_node", bitsPerNode);
        j.add("min", r.tiles() ? lo * fi.unit : NAN).add("max", r.tiles() ? hi * fi.unit : NAN);
        std::vector<double> eps;
        for (auto e : epsilons) eps.push_back(e * fi.unit);
        j.add("tile_epsilons", eps);
        std::vector<Json> grids;
        for (std::size_t g = 0; g < fi.grids.size(); ++g) {
            const GridInfo& gi = fi.grids[g];
            Json o;
            o.add("name", gi.name).add("crs", crsText(gi)).add("vertical_epsg", static_cast<unsigned>(gi.verticalEpsg)).add("vertical_unit", gi.verticalUnit);
            o.add("raster_type", rasterTypeText(gi.rasterType)).add("origin_x", gi.originX).add("origin_y", gi.originY);
            o.add("spacing_x", gi.spacingX).add("spacing_y", gi.spacingY).add("width", static_cast<unsigned>(gi.width)).add("height", static_cast<unsigned>(gi.height));
            o.add("tiles_x", static_cast<unsigned>(gi.tilesX)).add("tiles_y", static_cast<unsigned>(gi.tilesY));
            grids.push_back(o);
        }
        j.add("grids", grids);
        std::vector<Json> layers;
        for (std::size_t L = 0; L < r.layers(); ++L) {
            const LayerInfo& li = fi.layers[L];
            std::uint64_t bytes = 0;
            for (std::size_t k = 0; k < r.layerTiles(L); ++k) bytes += r.layerEntry(L, k).totalBytes;
            Json o;
            o.add("layer", static_cast<unsigned>(L + 1)).add("kind", static_cast<unsigned>(li.kind)).add("kind_name", kindName(li.kind));
            o.add("stride", static_cast<unsigned>(li.stride)).add("unit", li.unit).add("error_quanta", li.d).add("param", li.param);
            o.add("tiles", static_cast<std::uint64_t>(r.layerTiles(L))).add("tile_bytes", bytes);
            layers.push_back(o);
        }
        j.add("layers", layers).add("sections", static_cast<std::uint64_t>(r.sections())).add("sections_skipped", static_cast<std::uint64_t>(r.skippedSections()));
        if (a.has("tiles")) {
            std::vector<Json> tiles;
            for (std::size_t k = 0; k < r.tiles(); ++k) {
                const TileEntry e = r.entry(k);
                Json t;
                t.add("grid", static_cast<unsigned>(e.grid)).add("ti", static_cast<int>(e.ti)).add("tj", static_cast<int>(e.tj)).add("bytes", static_cast<unsigned>(e.totalBytes));
                t.add("head_bytes", static_cast<unsigned>(e.headBytes)).add("holes", (e.flags & kTileHoles) != 0);
                t.add("min", e.min * fi.unit).add("max", e.max * fi.unit).add("epsilon", e.d * fi.unit);
                tiles.push_back(t);
            }
            j.add("tile_list", tiles);
        }
        std::printf("%s\n", j.str().c_str());
        return 0;
    }

    std::printf("file          %s (%llu bytes, format version %u, method %u)\n", a.positional[0].c_str(),
                static_cast<unsigned long long>(fileBytes), kVersion, fi.method.bits());
    std::printf("parameters    tile %u, block %u, base %u, block levels %u\n", p.tile, p.block, p.base, p.blockLevels);
    std::printf("heights       unit %g, epsilon %g (%d quanta), no-data %g\n", fi.unit, fi.d * fi.unit, fi.d, fi.noData);
    if (!epsilons.empty()) {
        std::printf("tile epsilons");
        for (auto e : epsilons) std::printf(" %g", e * fi.unit);
        std::printf("\n");
    }
    std::printf("tiles         %zu (%llu with holes), heights %g to %g\n", r.tiles(), static_cast<unsigned long long>(holes),
                r.tiles() ? lo * fi.unit : NAN, r.tiles() ? hi * fi.unit : NAN);
    std::printf("bytes         header %zu, index %llu, tiles %llu (heads %llu)\n", r.headerBytes(), static_cast<unsigned long long>(r.indexBytes()),
                static_cast<unsigned long long>(tileBytes), static_cast<unsigned long long>(headBytes));
    std::printf("bits/node     %.4f (file bytes over S² nodes a tile)\n", bitsPerNode);
    std::printf("grids         %zu\n", fi.grids.size());
    for (std::size_t g = 0; g < fi.grids.size(); ++g) {
        const GridInfo& gi = fi.grids[g];
        std::printf("  %zu  %s  %s  %s, origin (%.12g, %.12g), spacing (%.12g, %.12g), %u × %u nodes, %u × %u tiles%s%s\n", g,
                    gi.name.c_str(), crsText(gi).c_str(), rasterTypeText(gi.rasterType), gi.originX, gi.originY, gi.spacingX,
                    gi.spacingY, gi.width, gi.height, gi.tilesX, gi.tilesY, gi.verticalUnit.empty() ? "" : ", vertical unit ",
                    gi.verticalUnit.c_str());
    }
    for (std::size_t L = 0; L < r.layers(); ++L) {
        const LayerInfo& li = fi.layers[L];
        std::uint64_t bytes = 0;
        for (std::size_t k = 0; k < r.layerTiles(L); ++k) bytes += r.layerEntry(L, k).totalBytes;
        std::printf("layer %zu       kind %u (%s), every %u nodes, quantum %g, epsilon %d quanta, parameter %g: %zu tiles, %llu bytes\n", L + 1,
                    static_cast<unsigned>(li.kind), kindName(li.kind), li.stride, li.unit, li.d, li.param, r.layerTiles(L), static_cast<unsigned long long>(bytes));
    }
    if (r.skippedSections()) std::printf("sections      %zu, %zu of types this reader does not know (skipped)\n", r.sections(), r.skippedSections());
    if (a.has("tiles"))
        for (std::size_t k = 0; k < r.tiles(); ++k) {
            const TileEntry e = r.entry(k);
            std::printf("  tile %u (%d, %d)  %u bytes (head %u)%s  %g to %g\n", e.grid, e.ti, e.tj, e.totalBytes, e.headBytes,
                        (e.flags & kTileHoles) ? " holes" : "", e.min * fi.unit, e.max * fi.unit);
        }
    return 0;
}

// ── encode ───────────────────────────────────────────────────────────────────

EncodeTotals encodeToFile(const FileInfo& info, const std::vector<TileJob>& jobs, const TileReader& read,
                          const TileCheck& check, unsigned threads, const std::string& path) {
    const auto t0 = std::chrono::steady_clock::now();
    const Params& p = info.params;
    FileSink sink(path);
    Builder builder;
    need(builder.begin(info, sink), "cannot start the file");

    // Encode on many threads; add to the file in the order of the jobs, so
    // that the same input always gives the same bytes.
    std::vector<std::optional<EncodedTile>> done(jobs.size());
    std::vector<char> ready(jobs.size(), 0);
    std::mutex m;
    std::size_t next = 0;
    EncodeTotals totals;
    Status addStatus = Status::Ok;
    parallelFor(jobs.size(), threads, [&](std::size_t k) {
        const TileJob& job = jobs[k];
        TileData data;
        const std::size_t count = read(job, data);
        std::optional<EncodedTile> t;
        if (count > 0) {
            t.emplace();
            std::vector<std::int32_t> recon(check ? p.nodes() : 0);
            const auto valid = count == p.nodes() ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>(data.valid);
            need(encodeTile(p, info.method, job.errorOr(info.d), data.x, valid, *t, recon), "cannot encode a tile");
            if (check) check(job, data, *t, recon);
        }
        std::lock_guard<std::mutex> lock(m);
        done[k] = std::move(t);
        ready[k] = 1;
        while (next < jobs.size() && ready[next]) {
            if (done[next]) {
                totals.validNodes += done[next]->valid;
                if (addStatus == Status::Ok)
                    addStatus = builder.add(jobs[next].grid, jobs[next].ti, jobs[next].tj, *done[next], jobs[next].errorOr(info.d));
                done[next].reset();
            } else {
                ++totals.emptyTiles;
            }
            ++next;
        }
    });
    need(addStatus, "cannot write a tile");
    totals.tiles = builder.tiles();
    need(builder.finish(), "cannot finish the file");
    if (!sink.close()) throw std::runtime_error("cannot write " + path);
    totals.seconds = seconds(t0, std::chrono::steady_clock::now());
    totals.bytes = MappedFile(path).bytes().size();
    return totals;
}

int cmdEncode(const Args& a) {
    if (a.has("layer-of")) return cmdAddLayer(a);
    if (a.positional.empty() || !a.has("output")) throw std::runtime_error("usage: pyre encode <sources…> -o <out.pyre>");
    const Params p = paramsOf(a);
    const double unit = a.number("unit", 0.01);
    if (!(unit >= 0.001 && unit <= 1)) throw std::runtime_error("--unit must be from 0.001 to 1");
    const std::int32_t d = errorQuanta(a.number("epsilon", 0.99), unit);
    const BBox box = bboxOf(a);
    const unsigned threads = threadsOf(a);

    std::vector<std::unique_ptr<Source>> sources;
    FileInfo info;
    info.params = p;
    info.method = Method::v1();
    info.d = d;
    info.unit = unit;
    std::vector<TileJob> jobs;
    for (const auto& path : a.positional) {
        sources.push_back(openSource(path));
        const SourceInfo& s = sources.back()->info();
        if (sources.size() == 1 && s.noData) info.noData = *s.noData;
        info.grids.push_back(gridOf(s, p));
        const GridInfo& g = info.grids.back();
        for (std::uint32_t tj = 0; tj < g.tilesY; ++tj)
            for (std::uint32_t ti = 0; ti < g.tilesX; ++ti)
                if (tileTouches(g, p, static_cast<std::int32_t>(ti), static_cast<std::int32_t>(tj), box))
                    jobs.push_back({static_cast<std::uint32_t>(info.grids.size() - 1), static_cast<std::int32_t>(ti), static_cast<std::int32_t>(tj)});
    }

    const auto read = [&](const TileJob& job, TileData& t) {
        std::vector<double> buf;
        return readTile(*sources[job.grid], p, unit, static_cast<std::uint32_t>(job.ti), static_cast<std::uint32_t>(job.tj), t.x, t.valid, buf);
    };
    const EncodeTotals r = encodeToFile(info, jobs, read, {}, threads, a.get("output"));
    const double bits = r.validNodes ? static_cast<double>(r.bytes) * 8 / static_cast<double>(r.validNodes) : 0.0;

    if (a.has("json")) {
        Json j;
        j.add("output", a.get("output")).add("sources", static_cast<std::uint64_t>(sources.size())).add("tiles", static_cast<std::uint64_t>(r.tiles));
        j.add("empty_tiles", static_cast<std::uint64_t>(r.emptyTiles)).add("valid_nodes", r.validNodes).add("bytes", r.bytes);
        j.add("bits_per_valid_node", bits).add("seconds", r.seconds).add("threads", threads);
        j.add("epsilon", d * unit).add("unit", unit);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("wrote %s: %zu tiles (%zu empty, not stored) from %zu sources, %llu bytes, %.4f bits a valid node, %.2f s on %u threads\n",
                    a.get("output").c_str(), r.tiles, r.emptyTiles, sources.size(), static_cast<unsigned long long>(r.bytes), bits, r.seconds, threads);
    }
    return 0;
}

// ── decode ───────────────────────────────────────────────────────────────────

int cmdDecode(const Args& a) {
    if (a.positional.size() != 1 || !a.has("output")) throw std::runtime_error("usage: pyre decode <file.pyre> -o <out.tif>");
    OpenFile f(a.positional[0]);
    const Reader& r = f.reader;
    const FileInfo& fi = r.info();
    const Params& p = fi.params;
    // --layer L (1 …): decode layer L of the file instead of the heights. Its
    // node (i, j) is node (stride·i, stride·j); its values are in its own unit.
    const std::uint32_t layer = a.count("layer", 0);
    if (layer > r.layers()) throw std::runtime_error("the file has " + std::to_string(r.layers()) + " layers; no layer " + a.get("layer"));
    const Params pl = layer ? r.layerParams(layer - 1) : p;
    const std::uint32_t rs = layer ? fi.layers[layer - 1].stride : 1;
    const double vunit = layer ? fi.layers[layer - 1].unit : fi.unit;
    auto findT = [&](std::uint32_t g, std::int32_t ti, std::int32_t tj) { return layer ? r.layerFind(layer - 1, g, ti, tj) : r.find(g, ti, tj); };
    auto entryT = [&](std::size_t k) { return layer ? r.layerEntry(layer - 1, k) : r.entry(k); };
    auto verifyT = [&](std::size_t k) { return layer ? r.verifyLayerTile(layer - 1, k) : r.verifyTile(k); };
    auto decodeT = [&](std::size_t k, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1, std::uint32_t st,
                       std::vector<std::int32_t>& out, std::vector<std::uint8_t>& mask, Scratch& s) {
        return layer ? r.decodeLayerWindow(layer - 1, k, i0, j0, i1, j1, st, out, mask, s) : r.decodeWindow(k, i0, j0, i1, j1, st, out, mask, s);
    };
    const std::uint32_t step = a.count("level", 1);
    if (step == 0 || (step & (step - 1)) != 0 || step > pl.base)
        throw std::runtime_error("--level must be a power of 2 from 1 to the base step " + std::to_string(pl.base));
    const BBox box = bboxOf(a);
    const unsigned threads = threadsOf(a);
    const float noData = static_cast<float>(a.number("nodata", std::isnan(fi.noData) ? NAN : fi.noData));

    std::vector<std::uint32_t> grids;
    if (a.has("grid")) {
        grids.push_back(a.count("grid", 0));
        if (grids[0] >= fi.grids.size()) throw std::runtime_error("no grid " + a.get("grid"));
    } else {
        for (std::uint32_t g = 0; g < fi.grids.size(); ++g) grids.push_back(g);
    }

    std::vector<Json> outputs;
    for (std::uint32_t gk : grids) {
        // A layer's grid: the same origin, every rs nodes.
        const GridInfo& g0 = fi.grids[gk];
        GridInfo g = g0;
        g.spacingX *= rs;
        g.spacingY *= rs;
        g.width = (g0.width - 1) / rs + 1;
        g.height = (g0.height - 1) / rs + 1;
        // The node range of the box, on the step's lattice.
        // The grid's nodes are first … first + nodes − 1 (first = the first tile · S).
        auto floorDiv = [](std::int64_t x, std::int64_t y) { return x >= 0 ? x / y : -((-x + y - 1) / y); };
        auto clampRange = [&](double lo, double hi, double origin, double spacing, std::int64_t first, std::uint32_t nodes,
                              std::int64_t& a0, std::int64_t& a1) {
            a0 = std::isinf(lo) ? first : static_cast<std::int64_t>(std::ceil((lo - origin) / spacing - 1e-9));
            a1 = std::isinf(hi) ? first + static_cast<std::int64_t>(nodes) - 1 : static_cast<std::int64_t>(std::floor((hi - origin) / spacing + 1e-9));
            a0 = std::max<std::int64_t>(a0, first);
            a1 = std::min<std::int64_t>(a1, first + static_cast<std::int64_t>(nodes) - 1);
            a0 = -floorDiv(-a0, step) * step;
            a1 = floorDiv(a1, step) * step;
        };
        std::int64_t I0, I1, J0, J1;
        clampRange(box.minX, box.maxX, g.originX, g.spacingX, static_cast<std::int64_t>(g.tileI0) * pl.tile, g.width, I0, I1);
        clampRange(box.minY, box.maxY, g.originY, g.spacingY, static_cast<std::int64_t>(g.tileJ0) * pl.tile, g.height, J0, J1);
        if (I0 > I1 || J0 > J1) continue;
        const auto w = static_cast<std::uint32_t>((I1 - I0) / step + 1), h = static_cast<std::uint32_t>((J1 - J0) / step + 1);
        std::vector<float> img(static_cast<std::size_t>(w) * h, noData);

        // The tiles that the range touches.
        std::vector<std::size_t> tiles;
        const std::int64_t lastI = static_cast<std::int64_t>(g.tileI0) + g.tilesX - 1, lastJ = static_cast<std::int64_t>(g.tileJ0) + g.tilesY - 1;
        for (std::int64_t tj = floorDiv(J0, pl.tile); tj <= std::min<std::int64_t>(floorDiv(J1, pl.tile), lastJ); ++tj)
            for (std::int64_t ti = floorDiv(I0, pl.tile); ti <= std::min<std::int64_t>(floorDiv(I1, pl.tile), lastI); ++ti)
                if (auto k = findT(gk, static_cast<std::int32_t>(ti), static_cast<std::int32_t>(tj))) tiles.push_back(*k);
        std::atomic<std::uint64_t> badTiles{0};
        parallelFor(tiles.size(), threads, [&](std::size_t n) {
            const std::size_t k = tiles[n];
            const TileEntry e = entryT(k);
            if (!verifyT(k)) {
                ++badTiles;
                return;
            }
            const std::int64_t tI = static_cast<std::int64_t>(e.ti) * pl.tile, tJ = static_cast<std::int64_t>(e.tj) * pl.tile;
            const std::uint32_t i0 = static_cast<std::uint32_t>(std::max<std::int64_t>(I0 - tI, 0)), i1 = static_cast<std::uint32_t>(std::min<std::int64_t>(I1 - tI, pl.tile));
            const std::uint32_t j0 = static_cast<std::uint32_t>(std::max<std::int64_t>(J0 - tJ, 0)), j1 = static_cast<std::uint32_t>(std::min<std::int64_t>(J1 - tJ, pl.tile));
            std::vector<std::int32_t> out(pl.nodes());
            std::vector<std::uint8_t> mask((e.flags & kTileHoles) ? pl.nodes() : 0);
            Scratch s;
            need(decodeT(k, i0, j0, i1, j1, step, out, mask, s), "cannot decode a tile");
            for (std::uint32_t j = j0; j <= j1; ++j) {
                const std::int64_t J = tJ + j;
                if ((J - J0) % step) continue;
                const auto row = static_cast<std::size_t>((J1 - J) / step);   // the first row is the northmost
                for (std::uint32_t i = i0; i <= i1; ++i) {
                    const std::int64_t I = tI + i;
                    if ((I - I0) % step) continue;
                    const std::size_t t = static_cast<std::size_t>(j) * pl.side() + i;
                    if (!mask.empty() && !mask[t]) continue;
                    img[row * w + static_cast<std::size_t>((I - I0) / step)] = static_cast<float>(double(out[t]) * vunit);
                }
            }
        });
        if (badTiles) throw std::runtime_error(std::to_string(badTiles.load()) + " tiles fail their checksum: the file is damaged");

        Georef geo;
        geo.model = static_cast<ModelType>(g.modelType);
        if (g.epsg) geo.epsg = g.epsg;
        geo.wkt = g.wkt;
        if (g.verticalEpsg) geo.verticalEpsg = g.verticalEpsg;
        geo.verticalUnit = g.verticalUnit;
        geo.rasterType = static_cast<RasterType>(g.rasterType == 1 ? 1 : 2);
        geo.spacingX = g.spacingX * step;
        geo.spacingY = g.spacingY * step;
        // The top-left node; for PixelIsArea, the corner of a pixel of the new
        // size with the same centre as the node's cell.
        geo.originX = g.originX + static_cast<double>(I0) * g.spacingX;
        geo.originY = g.originY + static_cast<double>(J1) * g.spacingY;
        if (geo.rasterType == RasterType::PixelIsArea) {
            geo.originX += g0.spacingX / 2 - geo.spacingX / 2;
            geo.originY += -g0.spacingY / 2 + geo.spacingY / 2;
        }
        std::string path = a.get("output");
        if (grids.size() > 1) {
            const auto dot = path.rfind('.');
            const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
            std::string name = g.name;
            const auto ndot = name.rfind('.');
            if (ndot != std::string::npos) name = name.substr(0, ndot);
            path = stem + "-" + (name.empty() ? std::to_string(gk) : name) + ".tif";
        }
        writeGeoTiff(path, w, h, img.data(), geo, std::isnan(noData) ? std::optional<double>() : std::optional<double>(noData));
        Json o;
        o.add("output", path).add("grid", static_cast<unsigned>(gk)).add("layer", static_cast<unsigned>(layer)).add("width", static_cast<unsigned>(w)).add("height", static_cast<unsigned>(h)).add("tiles", static_cast<std::uint64_t>(tiles.size()));
        outputs.push_back(o);
        if (!a.has("json"))
            std::printf("wrote %s: %u × %u at step %u from %zu tiles of grid %u\n", path.c_str(), w, h, step, tiles.size(), gk);
    }
    if (outputs.empty()) throw std::runtime_error("the box holds no node of the file");
    if (a.has("json")) {
        Json j;
        j.add("outputs", outputs).add("level", static_cast<unsigned>(step));
        std::printf("%s\n", j.str().c_str());
    }
    return 0;
}

// ── verify ───────────────────────────────────────────────────────────────────

int cmdVerify(const Args& a) {
    if (a.has("layer")) return verifyLayer(a);
    if (a.positional.size() < 2) throw std::runtime_error("usage: pyre verify <file.pyre> <sources…>");
    OpenFile f(a.positional[0]);
    const Reader& r = f.reader;
    const FileInfo& fi = r.info();
    const Params& p = fi.params;
    const unsigned threads = threadsOf(a);
    const auto t0 = std::chrono::steady_clock::now();

    // Sources by name: each grid of the file names its source.
    std::vector<std::unique_ptr<Source>> sources;
    std::vector<int> sourceOfGrid(fi.grids.size(), -1);
    for (std::size_t s = 1; s < a.positional.size(); ++s) {
        sources.push_back(openSource(a.positional[s]));
        const std::string name = baseName(a.positional[s]);
        for (std::size_t g = 0; g < fi.grids.size(); ++g)
            if (fi.grids[g].name == name) sourceOfGrid[g] = static_cast<int>(sources.size() - 1);
    }

    struct Job {
        std::uint32_t grid, ti, tj;
    };
    std::vector<Job> jobs;
    std::uint64_t gridsChecked = 0;
    for (std::uint32_t g = 0; g < fi.grids.size(); ++g) {
        if (sourceOfGrid[g] < 0) continue;
        ++gridsChecked;
        const SourceInfo& si = sources[static_cast<std::size_t>(sourceOfGrid[g])]->info();
        const GridInfo want = gridOf(si, p);
        if (want.width != fi.grids[g].width || want.height != fi.grids[g].height)
            throw std::runtime_error("grid " + fi.grids[g].name + " does not match its source's size");
        for (std::uint32_t tj = 0; tj < fi.grids[g].tilesY; ++tj)
            for (std::uint32_t ti = 0; ti < fi.grids[g].tilesX; ++ti) jobs.push_back({g, ti, tj});
    }
    if (gridsChecked == 0) throw std::runtime_error("no source matches a grid of the file (by file name)");

    std::mutex m;
    std::uint64_t checked = 0, breaches = 0, maskErrors = 0, missing = 0, badCrc = 0;
    std::int64_t worstQuanta = 0;      // against the source rounded to the quantum
    double worst = 0, sumSq = 0;       // against the float source, in vertical units
    double worstExcess = -INFINITY;    // worst (|error| − tile ε) in quanta
    parallelFor(jobs.size(), threads, [&](std::size_t n) {
        const Job& job = jobs[n];
        const Source& src = *sources[static_cast<std::size_t>(sourceOfGrid[job.grid])];
        std::vector<double> values;
        readTileValues(src, p, job.ti, job.tj, values);
        std::uint64_t valid = 0;
        for (double v : values) valid += !std::isnan(v);
        const auto k = r.find(job.grid, static_cast<std::int32_t>(job.ti), static_cast<std::int32_t>(job.tj));
        std::uint64_t c = 0, b = 0, me = 0, mi = 0, bc = 0;
        std::int64_t wq = 0;
        double w = 0, ss = 0, wx = -INFINITY;
        if (!k) {
            mi = valid > 0;   // a tile with data must be in the file
        } else if (!r.verifyTile(*k)) {
            bc = 1;
        } else {
            const TileSpec spec = r.spec(*k);
            std::vector<std::int32_t> out(p.nodes());
            std::vector<std::uint8_t> mask(spec.holes ? p.nodes() : 0);
            Scratch s;
            need(r.decodeWindow(*k, 0, 0, p.tile, p.tile, 1, out, mask, s), "cannot decode a tile");
            for (std::size_t t = 0; t < p.nodes(); ++t) {
                const bool srcValid = !std::isnan(values[t]);
                const bool decValid = mask.empty() || mask[t];
                if (srcValid != decValid) {
                    ++me;
                    continue;
                }
                if (!srcValid) continue;
                ++c;
                const std::int64_t e = std::abs(static_cast<std::int64_t>(out[t]) - toQuanta(values[t], fi.unit));
                wq = std::max(wq, e);
                wx = std::max(wx, static_cast<double>(e - spec.d));
                b += e > spec.d;
                const double em = std::abs(static_cast<double>(out[t]) * fi.unit - values[t]);
                w = std::max(w, em);
                ss += em * em;
            }
        }
        std::lock_guard<std::mutex> lock(m);
        checked += c;
        breaches += b;
        maskErrors += me;
        missing += mi;
        badCrc += bc;
        worstQuanta = std::max(worstQuanta, wq);
        worst = std::max(worst, w);
        worstExcess = std::max(worstExcess, wx);
        sumSq += ss;
    });
    const double rms = checked ? std::sqrt(sumSq / static_cast<double>(checked)) : 0;
    const bool ok = breaches == 0 && maskErrors == 0 && missing == 0 && badCrc == 0 && checked > 0;
    const double secs = seconds(t0, std::chrono::steady_clock::now());

    if (a.has("json")) {
        Json j;
        j.add("ok", ok).add("nodes_checked", checked).add("breaches", breaches).add("mask_errors", maskErrors);
        j.add("missing_tiles", missing).add("damaged_tiles", badCrc).add("grids_checked", gridsChecked);
        j.add("max_error_quanta", static_cast<std::int64_t>(worstQuanta)).add("max_error", worst).add("rms", rms);
        j.add("epsilon", fi.d * fi.unit).add("unit", fi.unit).add("seconds", secs);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("%s: %llu nodes checked in %llu grids; max error %lld quanta (rounded source), %.6g against the float source; RMS %.6g\n",
                    ok ? "ok" : "FAILED", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(gridsChecked), static_cast<long long>(worstQuanta), worst, rms);
        std::printf("epsilon %g (%d quanta of %g); the limit against the float source is ε + q/2 = %g\n", fi.d * fi.unit, fi.d, fi.unit,
                    fi.d * fi.unit + fi.unit / 2);
        if (!ok)
            std::printf("breaches %llu, mask errors %llu, missing tiles %llu, damaged tiles %llu\n", static_cast<unsigned long long>(breaches),
                        static_cast<unsigned long long>(maskErrors), static_cast<unsigned long long>(missing), static_cast<unsigned long long>(badCrc));
    }
    return ok ? 0 : 1;
}

}  // namespace pyre::tool
