// Tests of the local grid (tools/pyre/local.hpp), the sampler of many sources
// (resample.hpp) and the reader of the reference heights packs (alt.hpp).
// The source sheets are small GeoTIFFs that the test writes in a temporary folder.

#include <unistd.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alt.hpp"
#include "check.hpp"
#include "commands.hpp"
#include "files.hpp"
#include "local.hpp"
#include "overlay.hpp"
#include "pyre_source.hpp"
#include "resample.hpp"
#include "synth.hpp"
#include "tiff.hpp"
#include "tile_epsilon.hpp"

using namespace pyre;
using namespace pyre::tool;
namespace fs = std::filesystem;

namespace {

// ── Temporary files ──────────────────────────────────────────────────────────

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name)
        : path(fs::temp_directory_path() / ("pyre_test_resample_" + std::to_string(::getpid()) + "_" + name)) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        for (const char* f : {"a.tif", "b.tif", "c.tif", "pack.alt", "out.pyre", "a.pyre", "two.pyre", "ov.tif"}) fs::remove(path / f, ec);
        fs::remove(path, ec);
    }
    std::string file(const char* name) const { return (path / name).string(); }
};

constexpr double kPixel = 0.001;   // degrees

/// Writes a geographic sheet: pixel (col, row) is the cell with its north-west
/// corner at (west + col · kPixel, north − row · kPixel); value(lon, lat) at its centre.
template <class F>
void writeSheet(const std::string& path, double west, double north, std::uint32_t w, std::uint32_t h, F&& value,
                ModelType model = ModelType::Geographic) {
    std::vector<float> v(std::size_t(w) * h);
    for (std::uint32_t r = 0; r < h; ++r)
        for (std::uint32_t c = 0; c < w; ++c)
            v[std::size_t(r) * w + c] = float(value(west + (c + 0.5) * kPixel, north - (r + 0.5) * kPixel));
    Georef g;
    g.model = model;
    g.epsg = model == ModelType::Geographic ? 4326u : 25830u;
    g.rasterType = RasterType::PixelIsArea;
    g.originX = west;
    g.originY = north;
    g.spacingX = kPixel;
    g.spacingY = kPixel;
    writeGeoTiff(path, w, h, v.data(), g, -32767.0);
}

/// The value at one point (lon, lat) through a grid round (0.1, 0.1).
double sampleAt(const SheetSet& s, double lon, double lat) {
    const LocalGrid g(0.1, 0.1);
    std::vector<double> out;
    s.sample(g, g.xOf(lon), g.yOf(lat), 1.0, 1, out);
    return out[0];
}

// ── The local grid ───────────────────────────────────────────────────────────

TEST(local_grid_projection) {
    const LocalGrid g(40.5358125, -3.8213871);
    CHECK(g.lat0e7() == 405358125);
    CHECK(g.lon0e7() == -38213871);
    CHECK(g.latOf(0) == g.lat0());
    CHECK(g.lonOf(0) == g.lon0());
    // One degree of latitude is R · π/180 metres; of longitude, that times cos(lat0).
    const double perDegree = LocalGrid::kEarthRadius * LocalGrid::kPi / 180.0;
    CHECK(std::abs((g.latOf(10000) - g.lat0()) * perDegree - 10000) < 0.01);
    CHECK(std::abs((g.lonOf(10000) - g.lon0()) * perDegree * std::cos(g.lat0() * LocalGrid::kPi / 180) - 10000) < 0.01);
    // The inverse is good to the float rounding of the metres.
    for (double x : {-123456.0, -2.0, 0.0, 2.0, 98765.0}) {
        CHECK(std::abs(g.xOf(g.lonOf(x)) - x) < 0.02);
        CHECK(std::abs(g.yOf(g.latOf(x)) - x) < 0.02);
    }
    // The WKT gives the same grid back; another CRS gives none.
    const auto back = LocalGrid::fromWkt(g.wkt());
    REQUIRE(back.has_value());
    CHECK(*back == g);
    CHECK(!LocalGrid::fromWkt("GEOGCS[\"WGS 84\"]").has_value());
    CHECK(!(LocalGrid(40.5358126, -3.8213871) == g));
}

// ── The sampler ──────────────────────────────────────────────────────────────

TEST(bilinear_on_a_plane_is_exact) {
    TempDir dir("plane");
    auto plane = [](double lon, double lat) { return 500 + 1000 * lon - 2000 * lat; };
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 200, 200, plane);
    const SheetSet s({dir.file("a.tif")});
    REQUIRE(s.size() == 1);
    const LocalGrid g(0.1, 0.1);
    std::vector<double> out;
    const std::uint32_t side = 21;
    const double x0 = -5000, y0 = -5000, step = 500;
    REQUIRE(s.sample(g, x0, y0, step, side, out) == std::size_t(side) * side);
    double worst = 0;
    for (std::uint32_t j = 0; j < side; ++j)
        for (std::uint32_t i = 0; i < side; ++i) {
            const double want = plane(g.lonOf(x0 + i * step), g.latOf(y0 + j * step));
            worst = std::max(worst, std::abs(out[std::size_t(j) * side + i] - want));
        }
    CHECK(worst < 1e-3);   // float values near 500: about 3e-5
}

TEST(deepest_source_wins) {
    TempDir dir("deep");
    // a: lon 0 … 0.2, value 100; b: lon 0.1 … 0.3, value 200; both lat 0 … 0.2.
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 200, 200, [](double, double) { return 100.0; });
    writeSheet(dir.file("b.tif"), 0.1, 0.2, 200, 200, [](double, double) { return 200.0; });
    const SheetSet s({dir.file("b.tif"), dir.file("a.tif")});
    CHECK(sampleAt(s, 0.05, 0.1) == 100);    // a only
    CHECK(sampleAt(s, 0.12, 0.1) == 100);    // deeper in a (0.08) than in b (0.02)
    CHECK(sampleAt(s, 0.18, 0.1) == 200);    // deeper in b
    CHECK(sampleAt(s, 0.25, 0.1) == 200);    // b only
    CHECK(std::isnan(sampleAt(s, 0.35, 0.1)));   // no source
    CHECK(std::isnan(sampleAt(s, 0.1, 0.25)));   // north of both
}

TEST(no_data_falls_to_the_next_source) {
    TempDir dir("holes");
    // a has no data in lon 0.11 … 0.13; there b gives the value, though the point is deeper in a.
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 200, 200,
               [](double lon, double) { return lon > 0.11 && lon < 0.13 ? -32767.0 : 100.0; });
    writeSheet(dir.file("b.tif"), 0.1, 0.2, 200, 200, [](double, double) { return 200.0; });
    const SheetSet s({dir.file("a.tif"), dir.file("b.tif")});
    CHECK(sampleAt(s, 0.12, 0.1) == 200);
    CHECK(sampleAt(s, 0.14, 0.1) == 100);
    // With b gone, the hole has no data, and values below −1000 m count as none.
    const SheetSet a({dir.file("a.tif")});
    CHECK(std::isnan(sampleAt(a, 0.12, 0.1)));
    writeSheet(dir.file("c.tif"), 0.0, 0.2, 200, 200, [](double, double) { return -9999.0; });
    const SheetSet c({dir.file("c.tif")});
    CHECK(std::isnan(sampleAt(c, 0.05, 0.1)));
}

TEST(a_tie_goes_to_the_first_path) {
    TempDir dir("tie");
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 200, 200, [](double, double) { return 100.0; });
    writeSheet(dir.file("b.tif"), 0.0, 0.2, 200, 200, [](double, double) { return 200.0; });
    // The order on the command line does not count: the paths are sorted.
    const SheetSet s({dir.file("b.tif"), dir.file("a.tif")});
    CHECK(sampleAt(s, 0.05, 0.1) == 100);
    // A folder gives every .tif below it.
    const SheetSet f({dir.path.string()});
    CHECK(f.size() == 2);
    CHECK(sampleAt(f, 0.05, 0.1) == 100);
}

TEST(shared_sides_are_the_same) {
    TempDir dir("sides");
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 200, 200, [](double lon, double lat) { return 300 + 5000 * lon * lat; });
    writeSheet(dir.file("b.tif"), 0.1, 0.2, 200, 200, [](double lon, double lat) { return 300 + 5000 * lon * lat + 0.5; });
    const SheetSet s({dir.file("a.tif"), dir.file("b.tif")});
    const LocalGrid g(0.1, 0.1);
    // Two neighbour squares of 101 nodes every 20 m: the east side of one is the west side of the other.
    std::vector<double> west, east;
    const double tileM = 2000, step = 20;
    s.sample(g, -tileM, -tileM / 2, step, 101, west);
    s.sample(g, 0, -tileM / 2, step, 101, east);
    bool same = true;
    for (std::size_t j = 0; j < 101; ++j) same = same && west[j * 101 + 100] == east[j * 101];
    CHECK(same);
}

TEST(sources_must_be_geographic) {
    TempDir dir("crs");
    writeSheet(dir.file("a.tif"), 400000, 4500000, 50, 50, [](double, double) { return 1.0; }, ModelType::Projected);
    bool threw = false;
    try {
        const SheetSet s({dir.file("a.tif")});
    } catch (const SourceError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        const SheetSet s({dir.path.string() + "/none"});
    } catch (const SourceError&) {
        threw = true;
    }
    CHECK(threw);
}

// ── The reference packs ──────────────────────────────────────────────────────

struct PackTile {
    std::int32_t i, j, d;
    std::vector<std::int32_t> recon;
};

/// Writes a pack of tiles made with pyre's reference method.
void writePack(const std::string& path, std::vector<PackTile>& tiles, std::int32_t headerError, std::uint32_t version = 3) {
    const Params p = AltPack::params();
    std::vector<std::vector<std::uint8_t>> data;
    std::uint64_t seed = 7;
    for (auto& t : tiles) {
        const auto x = synth::terrain(synth::Kind::Mountain, 1001, ++seed, t.i, t.j);
        EncodedTile e;
        t.recon.assign(p.nodes(), 0);
        REQUIRE(encodeTile(p, Method::reference(), t.d, x, {}, e, t.recon) == Status::Ok);
        std::vector<std::uint8_t> b;
        auto u32 = [&](std::uint32_t v) {
            const auto* q = reinterpret_cast<const std::uint8_t*>(&v);
            b.insert(b.end(), q, q + 4);
        };
        u32(std::uint32_t(e.head.size()));
        b.insert(b.end(), e.head.begin(), e.head.end());
        for (const auto& blk : e.blocks) u32(std::uint32_t(blk.size()));
        for (const auto& blk : e.blocks) b.insert(b.end(), blk.begin(), blk.end());
        data.push_back(std::move(b));
    }
    std::vector<std::uint8_t> out(AltPack::kHeaderBytes + tiles.size() * AltPack::kEntryBytes);
    auto put = [&](std::size_t at, const auto& v) { std::memcpy(out.data() + at, &v, sizeof v); };
    put(0, AltPack::kMagic);
    put(4, version);
    put(8, 2000.0);
    put(16, 40.5358125);
    put(24, -3.8213871);
    put(32, headerError);
    put(36, std::uint32_t(1001));
    put(40, std::uint32_t(tiles.size()));
    for (std::size_t k = 0; k < tiles.size(); ++k) {
        const std::size_t e = AltPack::kHeaderBytes + k * AltPack::kEntryBytes;
        put(e, tiles[k].i);
        put(e + 4, tiles[k].j);
        put(e + 8, std::uint64_t(out.size()));
        put(e + 16, std::uint32_t(data[k].size()));
        put(e + 28, std::uint32_t(tiles[k].d == headerError ? 0 : tiles[k].d));
        out.insert(out.end(), data[k].begin(), data[k].end());
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

TEST(alt_pack_roundtrip) {
    TempDir dir("pack");
    std::vector<PackTile> tiles = {{-3, -2, 99, {}}, {5, -2, 9, {}}, {0, 4, 99, {}}};   // sorted by (j, i)
    writePack(dir.file("pack.alt"), tiles, 99);
    const AltPack pack(dir.file("pack.alt"));
    REQUIRE(pack.tiles() == 3);
    CHECK(pack.tileM() == 2000.0);
    CHECK(LocalGrid(pack.lat0(), pack.lon0()) == LocalGrid(40.5358125, -3.8213871));
    CHECK(pack.errorCm() == 99);
    Scratch s;
    for (std::size_t k = 0; k < tiles.size(); ++k) {
        const auto f = pack.find(tiles[k].i, tiles[k].j);
        REQUIRE(f.has_value());
        CHECK(*f == k);
        CHECK(pack.errorOf(k) == tiles[k].d);
        std::vector<std::int32_t> out;
        REQUIRE(pack.decode(k, out, s));
        CHECK(out == tiles[k].recon);
    }
    CHECK(!pack.find(1, 1).has_value());
}

TEST(alt_pack_damage) {
    TempDir dir("damage");
    std::vector<PackTile> tiles = {{0, 0, 99, {}}};
    writePack(dir.file("pack.alt"), tiles, 99, 2);
    bool threw = false;
    try {
        const AltPack pack(dir.file("pack.alt"));
    } catch (const SourceError&) {
        threw = true;
    }
    CHECK(threw);   // version 2 is refused

    writePack(dir.file("pack.alt"), tiles, 99);
    const auto full = fs::file_size(dir.file("pack.alt"));
    fs::resize_file(dir.file("pack.alt"), full - 100);
    threw = false;
    try {
        const AltPack pack(dir.file("pack.alt"));
    } catch (const SourceError&) {
        threw = true;
    }
    CHECK(threw);   // the tile is outside the file

    // A tile whose own lengths do not add up: decode says no, with no crash.
    writePack(dir.file("pack.alt"), tiles, 99);
    {
        std::fstream f(dir.file("pack.alt"), std::ios::in | std::ios::out | std::ios::binary);
        const std::uint32_t huge = 0x7FFFFFFF;
        f.seekp(std::streamoff(AltPack::kHeaderBytes + AltPack::kEntryBytes));
        f.write(reinterpret_cast<const char*>(&huge), 4);
    }
    const AltPack pack(dir.file("pack.alt"));
    Scratch s;
    std::vector<std::int32_t> out;
    CHECK(!pack.decode(0, out, s));
}

// ── Tiles with an error of their own ─────────────────────────────────────────

TEST(tile_epsilon_file) {
    const LocalGrid g(40.5358125, -3.8213871);
    const double tileM = 2000;
    // A box from about 100 m to 2500 m east and 110 m to 1500 m north of the centre: tiles (0, 0) and (1, 0).
    const std::string text = "# airfields\n"
                             "tile 5 -3 0.09\n"
                             "tile 5 -3 0.5   # the smaller stays\n"
                             "\n"
                             "box " + std::to_string(g.lon0() + 0.001) + " " + std::to_string(g.lat0() + 0.001) + " " +
                             std::to_string(g.lonOf(2500)) + " " + std::to_string(g.latOf(1500)) + " 0.2\n";
    const TileEpsilons m = parseTileEpsilons(text, g, tileM, 0.01);
    CHECK(m.size() == 3);
    CHECK(m.at({5, -3}) == 9);
    CHECK(m.at({0, 0}) == 20);
    CHECK(m.at({1, 0}) == 20);
    for (const char* bad : {"tile 1 2\n", "tile 1 2 0.1 extra\n", "box 1 1 0 0 0.1\n", "circle 0 0 1\n", "tile 1 2 -1\n"}) {
        bool threw = false;
        try {
            parseTileEpsilons(bad, g, tileM, 0.01);
        } catch (const SourceError&) {
            threw = true;
        }
        CHECK(threw);
    }
    TileEpsilons a = {{{0, 0}, 50}, {{2, 2}, 10}};
    mergeTileEpsilons(a, m);
    CHECK(a.at({0, 0}) == 20);
    CHECK(a.at({2, 2}) == 10);
    CHECK(a.size() == 4);
}

TEST(tile_epsilons_of_a_pack) {
    TempDir dir("packeps");
    std::vector<PackTile> tiles = {{-3, -2, 99, {}}, {5, -2, 9, {}}};
    writePack(dir.file("pack.alt"), tiles, 99);
    const TileEpsilons m = tileEpsilonsOfPack(dir.file("pack.alt"), 0.01);
    CHECK(m.size() == 1);
    CHECK(m.count({5, -2}) == 1 && m.at({5, -2}) == 9);
    CHECK(tileEpsilonsOfPack(dir.file("pack.alt"), 0.001).at({5, -2}) == 90);
}

TEST(encode_with_tile_epsilons) {
    TempDir dir("encode");
    const Params p{40, 20, 8, 1};
    FileInfo info;
    info.params = p;
    info.d = 99;
    info.unit = 0.01;
    GridInfo g;
    g.tileI0 = -1;
    g.tilesX = 3;
    g.tilesY = 1;
    g.width = 3 * p.tile + 1;
    g.height = p.tile + 1;
    info.grids.push_back(g);
    // Tile −1 with the file's ε, tile 0 with 5 quanta, tile 1 with 0 (lossless).
    const std::vector<TileJob> jobs = {{0, -1, 0, -1}, {0, 0, 0, 5}, {0, 1, 0, 0}};
    std::vector<std::vector<std::int32_t>> x;
    for (int k = 0; k < 3; ++k) x.push_back(synth::terrain(synth::Kind::Rough, int(p.side()), 40 + std::uint64_t(k), k, 0));
    const auto read = [&](const TileJob& job, TileData& t) {
        t.x = x[std::size_t(job.ti + 1)];
        t.valid.assign(p.nodes(), 1);
        return p.nodes();
    };
    std::int64_t worstOverLimit = INT64_MIN;
    const auto check = [&](const TileJob& job, const TileData& t, const EncodedTile&, const std::vector<std::int32_t>& recon) {
        std::int64_t w = 0;
        for (std::size_t n = 0; n < p.nodes(); ++n) w = std::max<std::int64_t>(w, std::abs(std::int64_t(recon[n]) - t.x[n]));
        worstOverLimit = std::max(worstOverLimit, w - job.errorOr(info.d));
    };
    encodeToFile(info, jobs, read, check, 2, dir.file("out.pyre"));
    CHECK(worstOverLimit <= 0);

    MappedFile map(dir.file("out.pyre"));
    Reader r;
    REQUIRE(r.open(map.bytes()) == Status::Ok);
    REQUIRE(r.tiles() == 3);
    const std::int32_t want[3] = {99, 5, 0};
    for (std::size_t k = 0; k < 3; ++k) {
        const TileEntry e = r.entry(k);
        CHECK(e.d == want[e.ti + 1]);
        std::vector<std::int32_t> out(p.nodes());
        std::vector<std::uint8_t> none;
        Scratch s;
        REQUIRE(r.decodeWindow(k, 0, 0, p.tile, p.tile, 1, out, none, s) == Status::Ok);
        std::int64_t w = 0;
        for (std::size_t n = 0; n < p.nodes(); ++n)
            w = std::max<std::int64_t>(w, std::abs(std::int64_t(out[n]) - x[std::size_t(e.ti + 1)][n]));
        CHECK(w <= e.d);
        if (e.d == 0) CHECK(w == 0);
    }
}

// ── A pyre file as a source ──────────────────────────────────────────────────

/// `pyre encode` of the given sources (quiet).
void encodeQuiet(const std::vector<std::string>& sources, const std::string& out, const std::string& epsilon,
                 const std::string& unit = "0.01") {
    Args a;
    a.positional = sources;
    a.options = {{"output", out}, {"epsilon", epsilon}, {"unit", unit}, {"tile", "40"}, {"block", "20"}, {"threads", "2"}, {"json", "1"}};
    std::fflush(stdout);
    const int saved = ::dup(1);
    std::FILE* null = std::fopen("/dev/null", "w");
    ::dup2(::fileno(null), 1);
    const int r = cmdEncode(a);
    std::fflush(stdout);
    ::dup2(saved, 1);
    ::close(saved);
    std::fclose(null);
    REQUIRE(r == 0);
}

TEST(pyre_file_reads_like_its_source) {
    TempDir dir("pyresrc");
    // 130 × 90 pixels, with a hole; values with millimetres, so the cm rounding shows.
    auto value = [](double lon, double lat) {
        return (lon > 0.05 && lon < 0.06 && lat > 0.15 && lat < 0.16) ? -32767.0 : 600 + 900 * lon * lon - 300 * lat + 0.0037;
    };
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 130, 90, value);
    encodeQuiet({dir.file("a.tif")}, dir.file("a.pyre"), "0");
    const auto tif = openSource(dir.file("a.tif"));
    const auto pyr = openSource(dir.file("a.pyre"));
    const SourceInfo &ti = tif->info(), &pi = pyr->info();
    CHECK(pi.width == ti.width && pi.height == ti.height);
    CHECK(pi.georef.model == ModelType::Geographic && pi.georef.rasterType == RasterType::PixelIsArea);
    CHECK(pi.georef.epsg == ti.georef.epsg);
    CHECK(std::abs(pi.georef.originX - ti.georef.originX) < 1e-12);
    CHECK(std::abs(pi.georef.originY - ti.georef.originY) < 1e-12);
    CHECK(pi.georef.spacingX == ti.georef.spacingX && pi.georef.spacingY == ti.georef.spacingY);
    // Lossless (ε = 0): each value is the source rounded to the cm, as a float32; holes are NaN.
    for (auto [c0, r0, w, h] : {std::array<std::uint32_t, 4>{0, 0, 130, 90}, {37, 11, 50, 41}, {129, 89, 1, 1}, {39, 0, 3, 90}}) {
        std::vector<double> a(std::size_t(w) * h), b(a.size());
        tif->readWindow(c0, r0, w, h, a.data());
        pyr->readWindow(c0, r0, w, h, b.data());
        bool same = true;
        for (std::size_t q = 0; q < a.size(); ++q) {
            if (ti.isNoData(a[q])) same = same && std::isnan(b[q]);
            else same = same && b[q] == double(float(std::round(a[q] * 100) / 100));
        }
        CHECK(same);
    }
    // The sampler gives the same within the cm rounding.
    const SheetSet fromTif({dir.file("a.tif")}), fromPyre({dir.file("a.pyre")});
    const LocalGrid g(0.1, 0.1);
    std::vector<double> x, y;
    fromTif.sample(g, g.xOf(0.002), g.yOf(0.13), 20, 101, x);
    fromPyre.sample(g, g.xOf(0.002), g.yOf(0.13), 20, 101, y);
    double worst = 0;
    bool holesSame = true;
    for (std::size_t q = 0; q < x.size(); ++q) {
        holesSame = holesSame && std::isnan(x[q]) == std::isnan(y[q]);
        if (!std::isnan(x[q]) && !std::isnan(y[q])) worst = std::max(worst, std::abs(x[q] - y[q]));
    }
    CHECK(holesSame);
    CHECK(worst <= 0.005 + 1e-4);

    // Values in whole mm, kept with a unit of 1 mm and ε = 0, read back bit for bit.
    writeSheet(dir.file("b.tif"), 0.0, 0.2, 130, 90, [&](double lon, double lat) {
        const double v = value(lon, lat);
        return v < -1000 ? v : std::round(v * 1000) / 1000;
    });
    encodeQuiet({dir.file("b.tif")}, dir.file("two.pyre"), "0", "0.001");
    const auto tb = openSource(dir.file("b.tif")), pb = openSource(dir.file("two.pyre"));
    std::vector<double> a(130 * 90), b(a.size());
    tb->readWindow(0, 0, 130, 90, a.data());
    pb->readWindow(0, 0, 130, 90, b.data());
    bool exact = true;
    for (std::size_t q = 0; q < a.size(); ++q) exact = exact && (tb->info().isNoData(a[q]) ? std::isnan(b[q]) : a[q] == b[q]);
    CHECK(exact);
}

TEST(pyre_file_with_many_grids) {
    TempDir dir("pyregrids");
    writeSheet(dir.file("a.tif"), 0.0, 0.2, 60, 50, [](double, double) { return 100.0; });
    writeSheet(dir.file("b.tif"), 0.05, 0.2, 70, 45, [](double, double) { return 200.0; });
    encodeQuiet({dir.file("a.tif"), dir.file("b.tif")}, dir.file("two.pyre"), "0.5");
    CHECK(PyreSource::gridCount(dir.file("two.pyre")) == 2);
    const auto b = openSource(dir.file("two.pyre") + "#1");
    CHECK(b->info().width == 70 && b->info().height == 45);
    std::vector<double> v(70);
    b->readWindow(0, 10, 70, 1, v.data());
    CHECK(std::abs(v[0] - 200) <= 0.5 && std::abs(v[69] - 200) <= 0.5);
    for (const std::string& bad : {dir.file("two.pyre"), dir.file("two.pyre") + "#2"}) {
        bool threw = false;
        try {
            openSource(bad);
        } catch (const SourceError&) {
            threw = true;
        }
        CHECK(threw);
    }
    // A set takes each grid as a source: a point inside both is deeper in b.
    const SheetSet s({dir.file("two.pyre")});
    CHECK(s.size() == 2);
    CHECK(std::abs(sampleAt(s, 0.075, 0.18) - 200) <= 0.5);
    CHECK(std::abs(sampleAt(s, 0.02, 0.18) - 100) <= 0.5);
}

// ── Overlays ─────────────────────────────────────────────────────────────────

void writeOverlay(const std::string& path, const LocalGrid& g, double x, double y, std::uint32_t w, std::uint32_t h,
                  const std::vector<float>& v, RasterType type = RasterType::PixelIsPoint, double spacing = 2,
                  std::string wkt = "", std::optional<std::uint32_t> epsg = {}) {
    Georef r;
    r.model = ModelType::Projected;
    r.wkt = wkt.empty() && !epsg ? g.wkt() : wkt;
    r.epsg = epsg;
    r.rasterType = type;
    r.originX = x;
    r.originY = y;
    r.spacingX = spacing;
    r.spacingY = spacing;
    writeGeoTiff(path, w, h, v.data(), r, -32767.0);
}

TEST(overlay_replaces_samples) {
    TempDir dir("overlay");
    const LocalGrid g(40.5, -3.7);
    const double step = 2;
    // 4 × 3 pixels, the north-west one at node (10, 20); one pixel with no data.
    std::vector<float> v = {1, 2, 3, 4, 5, -32767, 7, 8, 9, 10, 11, 12};
    writeOverlay(dir.file("ov.tif"), g, 10 * step, 20 * step, 4, 3, v);
    const Overlay o({dir.file("ov.tif")}, g, step);
    // A square of 10 × 10 nodes from (8, 15): NaN (no sample) except where the overlay has data.
    std::vector<double> sq(100, NAN);
    CHECK(o.apply(8, 15, 10, sq) == 11);
    auto at = [&](int I, int J) { return sq[std::size_t(J - 15) * 10 + std::size_t(I - 8)]; };
    CHECK(at(10, 20) == 1);    // the north-west pixel
    CHECK(at(13, 20) == 4);
    CHECK(at(10, 18) == 9);    // the south row
    CHECK(at(13, 18) == 12);
    CHECK(std::isnan(at(11, 19)));   // the no-data pixel leaves the sample
    CHECK(std::isnan(at(9, 20)));    // outside the overlay

    // The same with PixelIsArea: the cell's centre is on the node.
    writeOverlay(dir.file("ov.tif"), g, 10 * step - 1, 20 * step + 1, 4, 3, v, RasterType::PixelIsArea);
    const Overlay area({dir.file("ov.tif")}, g, step);
    std::vector<double> sq2(100, NAN);
    CHECK(area.apply(8, 15, 10, sq2) == 11);
    bool same = true;
    for (std::size_t q = 0; q < sq.size(); ++q) same = same && (sq[q] == sq2[q] || (std::isnan(sq[q]) && std::isnan(sq2[q])));
    CHECK(same);
}

TEST(overlay_must_be_on_the_grid) {
    TempDir dir("overlaybad");
    const LocalGrid g(40.5, -3.7);
    const std::vector<float> v(4, 1.0f);
    auto refused = [&](auto&& write) {
        write();
        try {
            const Overlay o({dir.file("ov.tif")}, g, 2);
        } catch (const SourceError&) {
            return true;
        }
        return false;
    };
    CHECK(refused([&] { writeOverlay(dir.file("ov.tif"), g, 21, 40, 2, 2, v); }));                                  // off the nodes
    CHECK(refused([&] { writeOverlay(dir.file("ov.tif"), g, 20, 40, 2, 2, v, RasterType::PixelIsPoint, 4); }));     // another step
    CHECK(refused([&] { writeOverlay(dir.file("ov.tif"), g, 20, 40, 2, 2, v, RasterType::PixelIsPoint, 2, "", 25830u); }));   // EPSG
    CHECK(refused([&] { writeOverlay(dir.file("ov.tif"), g, 20, 40, 2, 2, v, RasterType::PixelIsPoint, 2, LocalGrid(41, -3.7).wkt()); }));
    CHECK(!refused([&] { writeOverlay(dir.file("ov.tif"), g, 20, 40, 2, 2, v); }));
}

}  // namespace

CHECK_MAIN
