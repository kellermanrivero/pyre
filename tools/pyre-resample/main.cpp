// pyre-resample — samples geographic source rasters onto a local metre grid
// round a centre point, and encodes the grid into a pyre file.
//
// The grid and the projection are in tools/pyre/local.hpp; the sampling rule
// is in tools/pyre/resample.hpp. The file has one grid, with the projection as
// WKT. Each tile is checked when it is encoded: it is decoded again, and its
// error against the sampled values is measured.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>
#include <string>

#include "alt.hpp"
#include "commands.hpp"
#include "json.hpp"
#include "local.hpp"
#include "overlay.hpp"
#include "resample.hpp"
#include "tile_epsilon.hpp"

namespace {

using namespace pyre;
using namespace pyre::tool;

const char* kUsage = R"(pyre-resample 0.1 — sample source rasters onto a local metre grid, and encode it

usage:
  pyre-resample <sources or folders…> -o <out.pyre> --origin LAT,LON [options]

the grid:
  --origin LAT,LON   the centre of the grid, in degrees (kept to 1e-7°): the point
                     at 0, 0 metres. Not needed with --tiles-from (the pack has one).
  --spacing 2        metres between two nodes
  --tile 1000        cells a tile side; a tile is tile · spacing metres
which tiles (one of these; else all tiles that the sources touch):
  --tiles-from <pack.alt>              the tiles of a reference heights pack
  --bbox minX,minY,maxX,maxY           a box in metres on the grid
  --lonlat minLon,minLat,maxLon,maxLat a box in degrees
the codec:
  --epsilon 0.99 --unit 0.01 --block 200 --base 8 --block-levels 1
an error limit of their own for some tiles (the smallest wins where rules meet):
  --tile-epsilon <file>         lines "tile <i> <j> <epsilon>" or
                                "box <minLon> <minLat> <maxLon> <maxLat> <epsilon>"
  --tile-epsilon-from <pack.alt>  the tiles that have their own error in a pack
values that replace the samples (smoothed airfields, filled holes …):
  --overlay <raster or folder>  a raster on this grid (spacing = the step, pixels on the
                                nodes, the same local CRS or none); may be given again;
                                where overlays meet, the last one wins
other:
  --threads N        the worker threads (default: all cores)
  --open-limit 32    the most sources open at once
  --json             print one JSON object

The sources must be in degrees (a geographic CRS). A folder gives every .tif,
.tiff and .pyre below it; each grid of a .pyre file is a source, so an archive
made with `pyre encode` can stand in for the original rasters. Each node takes its value from the source where it is deepest
inside, if the 4 pixels round it have data (bilinear); else it has no data.
)";

bool takes(const std::string& o) {
    static const std::set<std::string> v = {"output", "origin", "spacing", "tile", "block", "base", "block-levels", "epsilon",
                                            "unit", "tiles-from", "bbox", "lonlat", "threads", "open-limit",
                                            "tile-epsilon", "tile-epsilon-from", "overlay"};
    return v.count(o) != 0;
}

std::int64_t floorDiv(double x, double tileM) { return std::int64_t(std::floor(x / tileM)); }

int run(const Args& a) {
    if (a.positional.empty() || !a.has("output")) throw std::runtime_error("give the sources and -o <out.pyre> (see --help)");
    Params p = paramsOf(a);
    const double unit = a.number("unit", 0.01);
    if (!(unit >= 0.001 && unit <= 1)) throw std::runtime_error("--unit must be from 0.001 to 1");
    const std::int32_t d = errorQuanta(a.number("epsilon", 0.99), unit);
    const double spacing = a.number("spacing", 2.0);
    if (!(spacing > 0) || !std::isfinite(spacing)) throw std::runtime_error("--spacing must be more than 0");
    const unsigned threads = threadsOf(a);
    // The same arithmetic as the reference packs: a tile is tileM metres, and a
    // node step is tileM / S.
    const double tileM = double(p.tile) * spacing;
    const double step = tileM / double(p.tile);

    std::unique_ptr<AltPack> pack;
    if (a.has("tiles-from")) pack = std::make_unique<AltPack>(a.get("tiles-from"));
    LocalGrid grid;
    if (a.has("origin")) {
        double lat = 0, lon = 0;
        if (std::sscanf(a.get("origin").c_str(), "%lf,%lf", &lat, &lon) != 2 || !(std::abs(lat) < 90) || !(std::abs(lon) <= 180))
            throw std::runtime_error("--origin needs LAT,LON in degrees");
        grid = LocalGrid(lat, lon);
    } else if (pack) {
        grid = LocalGrid(pack->lat0(), pack->lon0());
    } else {
        throw std::runtime_error("give --origin LAT,LON");
    }
    if (pack && !(grid == LocalGrid(pack->lat0(), pack->lon0()) && std::abs(pack->tileM() - tileM) < 1e-9))
        std::fprintf(stderr, "pyre-resample: warning: the pack's grid (%.7f, %.7f, tiles of %g m) is not this grid; its tile places are used as they are\n",
                     pack->lat0(), pack->lon0(), pack->tileM());

    const auto t0 = std::chrono::steady_clock::now();
    const SheetSet sheets(a.positional, a.count("open-limit", 32));
    const double headerSeconds = seconds(t0, std::chrono::steady_clock::now());

    // The tiles.
    std::vector<std::pair<std::int32_t, std::int32_t>> places;
    if (pack) {
        for (std::size_t k = 0; k < pack->tiles(); ++k) places.push_back({pack->entry(k).i, pack->entry(k).j});
    } else {
        double minX, minY, maxX, maxY;
        if (a.has("bbox")) {
            const BBox b = bboxOf(a, "bbox");
            minX = b.minX, minY = b.minY, maxX = b.maxX, maxY = b.maxY;
        } else {
            double minLon, minLat, maxLon, maxLat;
            if (a.has("lonlat")) {
                const BBox b = bboxOf(a, "lonlat");
                minLon = b.minX, minLat = b.minY, maxLon = b.maxX, maxLat = b.maxY;
            } else {
                sheets.bounds(minLon, minLat, maxLon, maxLat);
            }
            minX = grid.xOf(minLon), maxX = grid.xOf(maxLon), minY = grid.yOf(minLat), maxY = grid.yOf(maxLat);
        }
        const std::int64_t i0 = floorDiv(minX, tileM), i1 = floorDiv(maxX, tileM), j0 = floorDiv(minY, tileM), j1 = floorDiv(maxY, tileM);
        if ((i1 - i0 + 1) * (j1 - j0 + 1) > 50'000'000) throw std::runtime_error("the box holds too many tiles");
        for (std::int64_t j = j0; j <= j1; ++j)
            for (std::int64_t i = i0; i <= i1; ++i)
                if (sheets.touches(grid, double(i) * tileM, double(j) * tileM, step, std::uint32_t(p.side())))
                    places.push_back({std::int32_t(i), std::int32_t(j)});
    }
    if (places.empty()) throw std::runtime_error("no tile of the grid touches the sources");
    std::sort(places.begin(), places.end(), [](const auto& x, const auto& y) { return std::make_pair(x.second, x.first) < std::make_pair(y.second, y.first); });

    // The file: one grid, round the centre, with node (0, 0) at 0, 0 metres.
    FileInfo info;
    info.params = p;
    info.method = Method::v1();
    info.d = d;
    info.unit = unit;
    const SourceInfo& first = sheets.info(0);
    if (first.noData) info.noData = *first.noData;
    GridInfo g;
    std::int32_t ti0 = INT32_MAX, tj0 = INT32_MAX, ti1 = INT32_MIN, tj1 = INT32_MIN;
    for (const auto& [i, j] : places) {
        ti0 = std::min(ti0, i), ti1 = std::max(ti1, i), tj0 = std::min(tj0, j), tj1 = std::max(tj1, j);
    }
    g.epsg = 0;
    g.wkt = grid.wkt();
    g.verticalEpsg = first.georef.verticalEpsg.value_or(0);
    g.verticalUnit = first.georef.verticalUnit;
    g.rasterType = std::uint8_t(RasterType::PixelIsPoint);
    g.modelType = std::uint8_t(ModelType::Projected);
    g.tileI0 = ti0;
    g.tileJ0 = tj0;
    g.tilesX = std::uint32_t(ti1 - ti0 + 1);
    g.tilesY = std::uint32_t(tj1 - tj0 + 1);
    if (std::uint64_t(g.tilesX) * p.tile + 1 > 0xFFFFFFFFu || std::uint64_t(g.tilesY) * p.tile + 1 > 0xFFFFFFFFu)
        throw std::runtime_error("the grid is too large");
    g.width = g.tilesX * p.tile + 1;
    g.height = g.tilesY * p.tile + 1;
    g.originX = 0;
    g.originY = 0;
    g.spacingX = step;
    g.spacingY = step;
    char name[96];
    std::snprintf(name, sizeof name, "local %.7f,%.7f from %zu sources", grid.lat0(), grid.lon0(), sheets.size());
    g.name = name;
    info.grids.push_back(g);

    // The tiles with an error limit of their own.
    TileEpsilons own;
    if (a.has("tile-epsilon-from")) mergeTileEpsilons(own, tileEpsilonsOfPack(a.get("tile-epsilon-from"), unit));
    if (a.has("tile-epsilon")) mergeTileEpsilons(own, readTileEpsilons(a.get("tile-epsilon"), grid, tileM, unit));
    std::vector<TileJob> jobs;
    std::size_t ownJobs = 0;
    for (const auto& [i, j] : places) {
        const auto it = own.find({i, j});
        const std::int32_t td = it == own.end() || it->second == d ? -1 : it->second;
        jobs.push_back({0, i, j, td});
        ownJobs += td >= 0;
    }

    // The overlays: a folder gives its rasters in the order of their paths.
    std::vector<std::string> overlayPaths;
    for (const auto& o : a.values("overlay")) {
        std::error_code ec;
        if (std::filesystem::is_directory(o, ec)) {
            std::vector<std::string> in;
            for (const auto& e : std::filesystem::directory_iterator(o, ec)) {
                const std::string ext = sourceExtension(e.path().string());
                if (e.is_regular_file() && (ext == "tif" || ext == "tiff" || ext == "pyre")) in.push_back(e.path().string());
            }
            std::sort(in.begin(), in.end());
            overlayPaths.insert(overlayPaths.end(), in.begin(), in.end());
        } else {
            overlayPaths.push_back(o);
        }
    }
    const Overlay overlay(overlayPaths, grid, step);
    std::atomic<std::uint64_t> overlaid{0};

    const auto read = [&](const TileJob& job, TileData& t) -> std::size_t {
        sheets.sample(grid, double(job.ti) * tileM, double(job.tj) * tileM, step, std::uint32_t(p.side()), t.values);
        if (!overlay.empty())
            overlaid += overlay.apply(std::int64_t(job.ti) * p.tile, std::int64_t(job.tj) * p.tile, std::uint32_t(p.side()), t.values);
        t.x.assign(p.nodes(), 0);
        t.valid.assign(p.nodes(), 0);
        std::size_t count = 0;
        for (std::size_t n = 0; n < p.nodes(); ++n)
            if (!std::isnan(t.values[n])) {
                t.x[n] = toQuanta(t.values[n], unit);
                t.valid[n] = 1;
                ++count;
            }
        return count;
    };

    // The check of each tile: decoded again, and measured against the samples.
    std::mutex m;
    std::uint64_t checked = 0, breaches = 0, mismatches = 0;
    std::int64_t worstQuanta = 0, worstExcess = INT64_MIN;   // the largest error, and the largest error − the tile's limit
    double worst = 0, sumSq = 0;
    const auto check = [&](const TileJob& job, const TileData& t, const EncodedTile& e, const std::vector<std::int32_t>& recon) {
        const std::int32_t td = job.errorOr(d);
        thread_local Scratch s;
        thread_local std::vector<std::int32_t> back;
        thread_local std::vector<std::uint8_t> mask;
        back.assign(p.nodes(), 0);
        mask.assign(e.holes ? p.nodes() : 0, 0);
        const TileSpec spec{p, info.method, td, e.holes};
        bool same = decodeHead(spec, e.head, back, mask, 1, s) == Status::Ok;
        for (std::uint32_t b = 0; b < p.blocks() && same; ++b)
            same = decodeBlock(spec, e.blocks[b], back, mask, b % p.blocksPerSide(), b / p.blocksPerSide(), 1, s) == Status::Ok;
        std::uint64_t c = 0, br = 0;
        std::int64_t wq = 0;
        double w = 0, ss = 0;
        for (std::size_t n = 0; n < p.nodes(); ++n) {
            if (!t.valid[n]) continue;
            same = same && back[n] == recon[n] && (mask.empty() || mask[n]);
            const std::int64_t eq = std::abs(std::int64_t(recon[n]) - t.x[n]);
            wq = std::max(wq, eq);
            br += eq > td;
            const double em = std::abs(double(recon[n]) * unit - t.values[n]);
            w = std::max(w, em);
            ss += em * em;
            ++c;
        }
        std::lock_guard<std::mutex> lock(m);
        checked += c;
        breaches += br;
        mismatches += !same;
        worstQuanta = std::max(worstQuanta, wq);
        if (c) worstExcess = std::max(worstExcess, wq - td);
        worst = std::max(worst, w);
        sumSq += ss;
    };

    const EncodeTotals r = encodeToFile(info, jobs, read, check, threads, a.get("output"));
    const double total = seconds(t0, std::chrono::steady_clock::now());
    const double rms = checked ? std::sqrt(sumSq / double(checked)) : 0.0;
    const bool ok = breaches == 0 && mismatches == 0;
    const double bits = r.validNodes ? double(r.bytes) * 8 / double(r.validNodes) : 0.0;

    if (a.has("json")) {
        Json j;
        j.add("output", a.get("output")).add("ok", ok).add("sources", std::uint64_t(sheets.size()));
        j.add("origin_lat", grid.lat0()).add("origin_lon", grid.lon0()).add("spacing", step).add("tile_metres", tileM);
        j.add("tiles", std::uint64_t(r.tiles)).add("empty_tiles", std::uint64_t(r.emptyTiles));
        j.add("tile_i0", int(ti0)).add("tile_j0", int(tj0)).add("tiles_x", unsigned(g.tilesX)).add("tiles_y", unsigned(g.tilesY));
        j.add("valid_nodes", r.validNodes).add("bytes", r.bytes).add("bits_per_valid_node", bits);
        j.add("tiles_with_own_epsilon", std::uint64_t(ownJobs)).add("overlays", std::uint64_t(overlayPaths.size()));
        j.add("overlay_nodes", overlaid.load());
        j.add("nodes_checked", checked).add("breaches", breaches).add("decode_mismatches", mismatches);
        j.add("max_error_minus_tile_limit_quanta", std::int64_t(worstExcess));
        j.add("max_error_quanta", std::int64_t(worstQuanta)).add("max_error", worst).add("rms", rms);
        j.add("epsilon", d * unit).add("unit", unit).add("header_seconds", headerSeconds).add("seconds", total).add("threads", threads);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("pyre-resample: %zu sources (headers read in %.1f s), a local grid round %.7f, %.7f every %g m\n", sheets.size(),
                    headerSeconds, grid.lat0(), grid.lon0(), step);
        std::printf("wrote %s: %zu tiles (%zu with no data, not stored), %llu bytes, %.4f bits a valid node, %.1f s on %u threads\n",
                    a.get("output").c_str(), r.tiles, r.emptyTiles, (unsigned long long)r.bytes, bits, total, threads);
        if (ownJobs) std::printf("%zu tiles have an error limit of their own\n", ownJobs);
        if (!overlay.empty())
            std::printf("%zu overlays set %llu nodes (counted once a tile; a node on a shared side counts in each tile)\n",
                        overlayPaths.size(), (unsigned long long)overlaid.load());
        std::printf("%s: %llu nodes checked; max error %lld quanta (at most %+lld against its tile's limit), %.6g against the samples; RMS %.6g\n",
                    ok ? "ok" : "FAILED", (unsigned long long)checked, (long long)worstQuanta, (long long)worstExcess, worst, rms);
        if (!ok)
            std::printf("breaches %llu, tiles that decode differently %llu\n", (unsigned long long)breaches, (unsigned long long)mismatches);
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) {
        std::fputs(kUsage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    try {
        return run(parseArgs(argc, argv, 1, takes));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pyre-resample: %s\n", e.what());
        return 1;
    }
}
