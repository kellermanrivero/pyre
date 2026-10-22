// pyre compare: a file on a local grid (made by pyre-resample) against a
// reference heights pack (.alt) on the same grid. It decodes both, tile by
// tile, and gives the differences and the sizes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include "alt.hpp"
#include "commands.hpp"
#include "files.hpp"
#include "json.hpp"
#include "local.hpp"

namespace pyre::tool {

namespace {

/// The upper limits of the classes of |difference|, in metres; the last class is the rest.
constexpr double kClasses[] = {0.0, 0.01, 0.1, 0.5, 1.0, 2.0};
constexpr std::size_t kClassCount = sizeof kClasses / sizeof kClasses[0] + 1;

struct Stats {
    std::uint64_t tiles = 0, identicalTiles = 0, nodes = 0, equal = 0, noDataInFile = 0;
    // Bytes with the index entries: of all the tiles, and of the identical tiles only.
    std::uint64_t packBytes = 0, fileBytes = 0, packBytesIdentical = 0, fileBytesIdentical = 0;
    double maxDiff = 0, sumSq = 0;
    std::uint64_t classes[kClassCount] = {};

    void add(const Stats& o) {
        tiles += o.tiles;
        identicalTiles += o.identicalTiles;
        nodes += o.nodes;
        equal += o.equal;
        noDataInFile += o.noDataInFile;
        packBytes += o.packBytes;
        fileBytes += o.fileBytes;
        packBytesIdentical += o.packBytesIdentical;
        fileBytesIdentical += o.fileBytesIdentical;
        maxDiff = std::max(maxDiff, o.maxDiff);
        sumSq += o.sumSq;
        for (std::size_t c = 0; c < kClassCount; ++c) classes[c] += o.classes[c];
    }
    double rms() const { return nodes ? std::sqrt(sumSq / double(nodes)) : 0.0; }

    Json json() const {
        Json j;
        j.add("tiles", tiles).add("identical_tiles", identicalTiles).add("nodes_compared", nodes).add("nodes_equal", equal);
        j.add("nodes_no_data_in_file", noDataInFile).add("max_difference", maxDiff).add("rms_difference", rms());
        std::vector<Json> cls;
        for (std::size_t c = 0; c < kClassCount; ++c) {
            Json x;
            if (c < kClassCount - 1) x.add("up_to", kClasses[c]);
            else x.add("above", kClasses[kClassCount - 2]);
            x.add("nodes", classes[c]);
            cls.push_back(x);
        }
        j.add("classes", cls);
        Json sz;
        sz.add("pack_bytes", packBytes).add("file_bytes", fileBytes).add("ratio", packBytes ? double(fileBytes) / double(packBytes) : 0.0);
        sz.add("identical_pack_bytes", packBytesIdentical).add("identical_file_bytes", fileBytesIdentical);
        sz.add("identical_ratio", packBytesIdentical ? double(fileBytesIdentical) / double(packBytesIdentical) : 0.0);
        j.add("sizes", sz);
        return j;
    }

    void print(const char* title) const {
        std::printf("%s: %llu tiles (%llu identical); %llu nodes compared, %llu equal (%.4f %%); max |difference| %.3f m, RMS %.4f m",
                    title, (unsigned long long)tiles, (unsigned long long)identicalTiles, (unsigned long long)nodes,
                    (unsigned long long)equal, nodes ? 100.0 * double(equal) / double(nodes) : 0.0, maxDiff, rms());
        if (noDataInFile) std::printf("; %llu nodes have no data in the file", (unsigned long long)noDataInFile);
        std::printf("\n  |difference|:");
        for (std::size_t c = 0; c < kClassCount; ++c) {
            if (c == 0) std::printf(" 0: %llu", (unsigned long long)classes[c]);
            else if (c < kClassCount - 1) std::printf(", ≤ %g m: %llu", kClasses[c], (unsigned long long)classes[c]);
            else std::printf(", > %g m: %llu", kClasses[c - 1], (unsigned long long)classes[c]);
        }
        std::printf("\n  sizes (with the index entries): pack %llu bytes, file %llu bytes, ratio %.4f",
                    (unsigned long long)packBytes, (unsigned long long)fileBytes, packBytes ? double(fileBytes) / double(packBytes) : 0.0);
        if (identicalTiles)
            std::printf("; identical tiles only: pack %llu, file %llu, ratio %.4f", (unsigned long long)packBytesIdentical,
                        (unsigned long long)fileBytesIdentical, double(fileBytesIdentical) / double(packBytesIdentical));
        std::printf("\n");
    }
};

}  // namespace

int cmdCompare(const Args& a) {
    if (a.positional.size() != 2) throw std::runtime_error("usage: pyre compare <file.pyre> <pack.alt>");
    const std::string filePath = a.positional[0], packPath = a.positional[1];
    MappedFile map(filePath);
    Reader r;
    if (const Status st = r.open(map.bytes()); st != Status::Ok)
        throw std::runtime_error(filePath + ": " + toString(st) + ": " + r.error());
    const AltPack pack(packPath);
    const FileInfo& fi = r.info();
    const Params& p = fi.params;
    const unsigned threads = threadsOf(a);

    // The grid: the one given, or the first on the pack's local grid.
    const LocalGrid want(pack.lat0(), pack.lon0());
    const double step = pack.tileM() / double(AltPack::kNodes - 1);
    auto onPackGrid = [&](const GridInfo& g) {
        const auto lg = LocalGrid::fromWkt(g.wkt);
        return lg && *lg == want && g.originX == 0 && g.originY == 0 && std::abs(g.spacingX - step) < 1e-9 &&
               std::abs(g.spacingY - step) < 1e-9;
    };
    std::optional<std::uint32_t> grid;
    if (a.has("grid")) {
        grid = a.count("grid", 0);
        if (*grid >= fi.grids.size()) throw std::runtime_error("no grid " + a.get("grid"));
        if (!onPackGrid(fi.grids[*grid])) throw std::runtime_error("grid " + a.get("grid") + " is not on the pack's grid");
    } else {
        for (std::uint32_t g = 0; g < fi.grids.size() && !grid; ++g)
            if (onPackGrid(fi.grids[g])) grid = g;
    }
    if (!grid) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "no grid of the file is on the pack's grid (a local grid round %.7f, %.7f, every %g m); make the file "
                      "with pyre-resample --origin %.7f,%.7f --spacing %g",
                      want.lat0(), want.lon0(), step, want.lat0(), want.lon0(), step);
        throw std::runtime_error(b);
    }
    if (p.tile != AltPack::kNodes - 1)
        throw std::runtime_error("the file's tiles have " + std::to_string(p.tile) + " cells a side; the pack's have 1000");

    // The tiles in both, and in one only.
    struct Pair {
        std::size_t packTile, fileTile;
    };
    std::vector<Pair> pairs;
    std::uint64_t packOnly = 0, packBytesSame = 0, fileBytesSame = 0;
    for (std::size_t k = 0; k < pack.tiles(); ++k) {
        const AltEntry& e = pack.entry(k);
        if (auto f = r.find(*grid, e.i, e.j)) {
            pairs.push_back({k, *f});
            packBytesSame += e.bytes + AltPack::kEntryBytes;
            fileBytesSame += r.entry(*f).totalBytes + kEntryBytes + 4 * p.blocks();
        } else {
            ++packOnly;
        }
    }
    std::uint64_t fileTilesOfGrid = 0;
    for (std::size_t k = 0; k < r.tiles(); ++k) fileTilesOfGrid += r.entry(k).grid == *grid;
    const std::uint64_t fileOnly = fileTilesOfGrid - pairs.size();
    if (pairs.empty()) throw std::runtime_error("the file and the pack have no tile in common");

    // Decode both and compare. The pack's tiles with their own error (finer, for
    // example under airfields) are counted apart: their heights were changed
    // before they were encoded.
    Stats same, own;
    std::uint64_t damaged = 0, epsilonDiffers = 0;
    std::mutex m;
    parallelFor(pairs.size(), threads, [&](std::size_t n) {
        const Pair& pr = pairs[n];
        thread_local Scratch s;
        std::vector<std::int32_t> alt, out(p.nodes());
        const TileEntry e = r.entry(pr.fileTile);
        std::vector<std::uint8_t> mask((e.flags & kTileHoles) ? p.nodes() : 0);
        const bool ok = pack.decode(pr.packTile, alt, s) && r.verifyTile(pr.fileTile) &&
                        r.decodeWindow(pr.fileTile, 0, 0, p.tile, p.tile, 1, out, mask, s) == Status::Ok;
        Stats t;
        if (ok) {
            t.tiles = 1;
            for (std::size_t q = 0; q < p.nodes(); ++q) {
                if (!mask.empty() && !mask[q]) {
                    ++t.noDataInFile;
                    continue;
                }
                const double diff = std::abs(double(out[q]) * fi.unit - double(alt[q]) * 0.01);
                const bool eq = diff < 1e-9;
                ++t.nodes;
                t.equal += eq;
                t.maxDiff = std::max(t.maxDiff, diff);
                t.sumSq += diff * diff;
                std::size_t c = 0;
                if (!eq)
                    for (c = 1; c < kClassCount - 1 && diff > kClasses[c] + 1e-9; ++c) {
                    }
                ++t.classes[c];
            }
            t.identicalTiles = t.equal == p.nodes();
            t.packBytes = pack.entry(pr.packTile).bytes + AltPack::kEntryBytes;
            t.fileBytes = e.totalBytes + kEntryBytes + 4 * p.blocks();
            if (t.identicalTiles) {
                t.packBytesIdentical = t.packBytes;
                t.fileBytesIdentical = t.fileBytes;
            }
        }
        const bool ownError = pack.entry(pr.packTile).errorCm != 0 && pack.errorOf(pr.packTile) != pack.errorCm();
        std::lock_guard<std::mutex> lock(m);
        if (!ok) ++damaged;
        else (ownError ? own : same).add(t);
        epsilonDiffers += std::abs(double(e.d) * fi.unit - double(pack.errorOf(pr.packTile)) * 0.01) > 1e-9;
    });
    Stats all = same;
    all.add(own);

    if (a.has("json")) {
        Json j;
        j.add("file", filePath).add("pack", packPath).add("grid", unsigned(*grid));
        j.add("origin_lat", want.lat0()).add("origin_lon", want.lon0()).add("spacing", step);
        j.add("file_epsilon", fi.d * fi.unit).add("pack_epsilon", pack.errorCm() * 0.01);
        j.add("tiles_in_both", std::uint64_t(pairs.size())).add("tiles_in_pack_only", packOnly).add("tiles_in_file_only", fileOnly);
        j.add("damaged_tiles", damaged).add("tiles_with_another_epsilon", epsilonDiffers);
        j.add("all", all.json()).add("pack_epsilon_tiles", same.json()).add("own_epsilon_tiles", own.json());
        Json sz;
        sz.add("pack_bytes", packBytesSame).add("file_bytes", fileBytesSame).add("ratio", double(fileBytesSame) / double(packBytesSame));
        sz.add("pack_file_bytes", pack.fileBytes()).add("file_file_bytes", std::uint64_t(map.bytes().size()));
        j.add("sizes_of_tiles_in_both", sz);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("grid %u of %s and %s: a local grid round %.7f, %.7f, every %g m\n", *grid, filePath.c_str(), packPath.c_str(),
                    want.lat0(), want.lon0(), step);
        std::printf("epsilon: file %g m, pack %g m; %llu tiles have another epsilon in each\n", fi.d * fi.unit, pack.errorCm() * 0.01,
                    (unsigned long long)epsilonDiffers);
        std::printf("tiles: %zu in both, %llu in the pack only, %llu in the file only, %llu damaged\n", pairs.size(),
                    (unsigned long long)packOnly, (unsigned long long)fileOnly, (unsigned long long)damaged);
        all.print("all tiles in both");
        if (own.tiles) {
            same.print("tiles with the pack's epsilon");
            own.print("tiles with their own epsilon in the pack");
        }
        std::printf("sizes of the tiles in both (with their index entries): pack %llu bytes, file %llu bytes, ratio %.4f\n",
                    (unsigned long long)packBytesSame, (unsigned long long)fileBytesSame, double(fileBytesSame) / double(packBytesSame));
        std::printf("whole files: pack %llu bytes, file %llu bytes\n", (unsigned long long)pack.fileBytes(),
                    (unsigned long long)map.bytes().size());
    }
    return damaged ? 1 : 0;
}

}  // namespace pyre::tool
