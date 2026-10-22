// Tests of the file: the checksum, write and read, the index, windows,
// damage, and the golden files (bit-exact on every platform).

#include <cstdio>
#include <fstream>
#include <iterator>

#include "check.hpp"
#include "pyre/pyre.hpp"
#include "synth.hpp"

using namespace pyre;

#ifndef PYRE_GOLDEN_DIR
#define PYRE_GOLDEN_DIR "tests/golden"
#endif

namespace {

TEST(crc32c_values) {
    const char* s = "123456789";
    CHECK(crc32c(s, 9) == 0xE3069283u);
    CHECK(crc32c(s, 4, crc32c(s + 4, 0)) == crc32c(s, 4));
    CHECK(crc32c(s + 4, 5, crc32c(s, 4)) == 0xE3069283u);
    CHECK(crc32c(nullptr, 0) == 0);
    std::vector<std::uint8_t> big(10007);
    synth::Rng r(3);
    for (auto& b : big) b = std::uint8_t(r.next());
    for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 63u, 10007u})
        CHECK(crc32c(big.data(), n) == ~detail::crc32cTable(big.data(), n, ~0u));
}

/// Integer-only terrain: the same on every platform (the golden files need it).
std::vector<std::int32_t> integerTerrain(const Params& p, std::uint64_t seed, int gi = 0, int gj = 0) {
    const std::size_t side = p.side();
    std::vector<std::int32_t> x(side * side);
    for (std::size_t j = 0; j < side; ++j)
        for (std::size_t i = 0; i < side; ++i) {
            const std::int64_t gx = gi * std::int64_t(p.tile) + std::int64_t(i), gy = gj * std::int64_t(p.tile) + std::int64_t(j);
            // A parabola bowl plus ridges and a hash for roughness.
            std::uint64_t z = std::uint64_t(gx) * 0x9E3779B1u ^ std::uint64_t(gy) * 0x85EBCA77u ^ seed;
            z = (z ^ (z >> 29)) * 0xBF58476D1CE4E5B9ull;
            const std::int64_t ridge = ((gx + 2 * gy) % 97) * 37 - ((3 * gx - gy) % 61) * 23;
            x[j * side + i] = std::int32_t(80000 + ((gx - 40) * (gx - 40) + (gy - 70) * (gy - 70)) / 3 + ridge + std::int64_t(z % 150));
        }
    return x;
}

struct Built {
    std::vector<std::uint8_t> file;
    std::vector<std::vector<std::int32_t>> recon;   // by tile, in the order added
    std::vector<std::vector<std::uint8_t>> masks;
};

/// Two grids: grid 0 with 3 × 2 tiles (one empty), grid 1 with one tile.
Built buildFile(const Params& p, Method m, std::int32_t d, bool holes) {
    FileInfo info;
    info.params = p;
    info.method = m;
    info.d = d;
    info.unit = 0.01;
    info.noData = -32767;
    GridInfo g0;
    g0.epsg = 4326;
    g0.modelType = 2;
    g0.originX = -1.190594;
    g0.originY = 42.9134;
    g0.spacingX = g0.spacingY = 0.000018;
    g0.width = 3 * p.tile + 1;
    g0.height = 2 * p.tile + 1;
    g0.tilesX = 3;
    g0.tilesY = 2;
    g0.verticalUnit = "metre";
    g0.name = "MDT02-WGS84-0117-1-COB2.tif";
    GridInfo g1 = g0;
    g1.epsg = 0;
    g1.wkt = "LOCAL_CS[\"test\"]";
    g1.name = "second";
    g1.tilesX = g1.tilesY = 1;
    info.grids = {g0, g1};

    VectorSink sink;
    Builder b;
    REQUIRE(b.begin(info, sink) == Status::Ok);
    Built out;
    // Add in a scrambled order: the index sorts them.
    const int places[][3] = {{0, 2, 1}, {1, 0, 0}, {0, 0, 0}, {0, 1, 1}, {0, 2, 0}, {0, 1, 0}};
    for (const auto& pl : places) {
        auto x = integerTerrain(p, 9, pl[1], pl[2]);
        std::vector<std::uint8_t> mask;
        if (holes) {
            mask.assign(p.nodes(), 1);
            for (std::size_t k = 0; k < mask.size(); k += 7 + std::size_t(pl[1])) mask[k] = 0;
        }
        EncodedTile t;
        std::vector<std::int32_t> recon(p.nodes());
        // A per-tile error limit for one tile.
        const std::int32_t td = (pl[0] == 0 && pl[1] == 1 && pl[2] == 1) ? d / 2 : d;
        REQUIRE(encodeTile(p, m, td, x, mask, t, recon) == Status::Ok);
        REQUIRE(b.add(std::uint32_t(pl[0]), pl[1], pl[2], t, td) == Status::Ok);
        out.recon.push_back(std::move(recon));
        out.masks.push_back(std::move(mask));
    }
    REQUIRE(b.finish() == Status::Ok);
    out.file = std::move(sink.bytes);
    return out;
}

TEST(file_roundtrip) {
    for (const Params& p : {Params{40, 20, 8, 1}, Params{64, 16, 16, 2}})
        for (bool holes : {false, true}) {
            const Built b = buildFile(p, Method::v1(), 99, holes);
            Reader r;
            REQUIRE(r.open(b.file) == Status::Ok);
            CHECK(r.tiles() == 6);
            CHECK(r.info().params == p);
            CHECK(r.info().d == 99 && r.info().unit == 0.01 && r.info().noData == -32767);
            REQUIRE(r.info().grids.size() == 2);
            CHECK(r.info().grids[0].name == "MDT02-WGS84-0117-1-COB2.tif" && r.info().grids[0].verticalUnit == "metre");
            CHECK(r.info().grids[1].wkt == "LOCAL_CS[\"test\"]" && r.info().grids[1].epsg == 0);
            CHECK(r.info().grids[0].originX == -1.190594 && r.info().grids[0].spacingY == 0.000018);
            // Sorted by grid, row, column.
            for (std::size_t k = 1; k < r.tiles(); ++k) {
                const TileEntry a = r.entry(k - 1), e = r.entry(k);
                CHECK(detail::entryLess(a, e.grid, e.ti, e.tj));
            }
            CHECK(!r.find(0, 0, 1).has_value());
            CHECK(!r.find(2, 0, 0).has_value());
            const int places[][3] = {{0, 2, 1}, {1, 0, 0}, {0, 0, 0}, {0, 1, 1}, {0, 2, 0}, {0, 1, 0}};
            Scratch s;
            for (std::size_t t = 0; t < 6; ++t) {
                const auto k = r.find(std::uint32_t(places[t][0]), places[t][1], places[t][2]);
                REQUIRE(k.has_value());
                CHECK(r.verifyTile(*k));
                const TileEntry e = r.entry(*k);
                CHECK(((e.flags & kTileHoles) != 0) == holes);
                CHECK(e.d == ((places[t][0] == 0 && places[t][1] == 1 && places[t][2] == 1) ? 49 : 99));
                std::vector<std::int32_t> out(p.nodes());
                std::vector<std::uint8_t> mask(holes ? p.nodes() : 0);
                REQUIRE(r.decodeWindow(*k, 0, 0, p.tile, p.tile, 1, out, mask, s) == Status::Ok);
                CHECK(out == b.recon[t]);
                if (holes) CHECK(mask == b.masks[t]);
                std::int32_t lo = INT32_MAX, hi = INT32_MIN;
                for (std::size_t n = 0; n < out.size(); ++n)
                    if (!holes || mask[n]) lo = std::min(lo, out[n]), hi = std::max(hi, out[n]);
                CHECK(e.min == lo && e.max == hi);
            }
        }
}

/// A window decodes only the blocks it touches: the others stay as they were.
TEST(file_window_touches_only_its_blocks) {
    const Params p{40, 10, 4, 1};
    const Built b = buildFile(p, Method::v1(), 20, false);
    Reader r;
    REQUIRE(r.open(b.file) == Status::Ok);
    const std::size_t k = *r.find(0, 0, 0);
    const std::size_t t = 2;   // tile (0, 0, 0) was the third added
    Scratch s;
    std::vector<std::int32_t> out(p.nodes(), INT32_MIN);
    std::vector<std::uint8_t> none;
    REQUIRE(r.decodeWindow(k, 12, 3, 18, 9, 1, out, none, s) == Status::Ok);   // inside block (1, 0)
    for (std::size_t j = 0; j <= p.tile; ++j)
        for (std::size_t i = 0; i <= p.tile; ++i) {
            const std::size_t n = j * p.side() + i;
            const bool inBlock10 = i > 10 && i < 20 && j > 0 && j < 10;
            const bool head = i % 2 == 0 && j % 2 == 0;
            const bool line = i % 10 == 0 || j % 10 == 0;
            if (inBlock10 || head || line) CHECK(out[n] == b.recon[t][n]);
            else CHECK(out[n] == INT32_MIN);
        }
}

TEST(file_bit_exact_bytes) {
    // The same input gives the same file, byte for byte, every time.
    const Built a = buildFile(Params{40, 20, 8, 1}, Method::v1(), 99, true);
    const Built b = buildFile(Params{40, 20, 8, 1}, Method::v1(), 99, true);
    CHECK(a.file == b.file);
}

TEST(file_rejects_damage) {
    const Built b = buildFile(Params{40, 20, 8, 1}, Method::v1(), 99, true);
    Reader r;
    synth::Rng rng(17);
    int opened = 0, caught = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        std::vector<std::uint8_t> bad = b.file;
        switch (trial % 4) {
            case 0: bad.resize(std::size_t(rng.range(0, int(bad.size()) - 1))); break;
            case 1: bad[std::size_t(rng.range(0, int(bad.size()) - 1))] ^= std::uint8_t(1u << rng.range(0, 7)); break;
            case 2:
                for (int k = 0; k < 8; ++k) bad[std::size_t(rng.range(0, int(bad.size()) - 1))] = std::uint8_t(rng.next());
                break;
            case 3:
                for (auto& x : bad) x = std::uint8_t(rng.next());
                break;
        }
        const Status st = r.open(bad);
        if (st != Status::Ok) {
            CHECK(st == Status::Corrupt || st == Status::Version);
            CHECK(!r.error().empty());
            continue;
        }
        ++opened;
        // The header and index are whole: a flipped tile byte must fail its
        // checksum, and decoding it anyway must not crash.
        Scratch s;
        for (std::size_t k = 0; k < r.tiles(); ++k) {
            const bool ok = r.verifyTile(k);
            caught += !ok;
            std::vector<std::int32_t> out(r.info().params.nodes());
            std::vector<std::uint8_t> mask(r.info().params.nodes());
            const Status d = r.decodeWindow(k, 0, 0, 40, 40, 1, out, mask, s);
            CHECK(d == Status::Ok || d == Status::Corrupt);
        }
    }
    // Every damaged file that opened had a damaged tile that its checksum found
    // (or the damage was in padding): count, do not require.
    std::printf("  damaged files that opened: %d; tiles caught by their checksum: %d\n", opened, caught);
    // Not a pyre file, or a newer version.
    std::vector<std::uint8_t> v = b.file;
    v[4] = 2;
    CHECK(r.open(v) == Status::Version);
    CHECK(r.error().find("version 2") != std::string::npos);
    CHECK(r.open(std::span<const std::uint8_t>{}) == Status::Corrupt);
}

TEST(file_rejects_two_tiles_at_one_place) {
    const Params p{8, 4, 4, 1};
    FileInfo info;
    info.params = p;
    info.grids.resize(1);
    VectorSink sink;
    Builder b;
    REQUIRE(b.begin(info, sink) == Status::Ok);
    std::vector<std::int32_t> x(p.nodes(), 7);
    EncodedTile t;
    REQUIRE(encodeTile(p, Method::v1(), 0, x, {}, t) == Status::Ok);
    CHECK(b.add(0, 1, 1, t, 0) == Status::Ok);
    CHECK(b.add(0, 1, 1, t, 0) == Status::Ok);
    CHECK(b.finish() == Status::BadInput);
}

TEST(file_empty_index) {
    FileInfo info;
    info.grids.resize(1);
    VectorSink sink;
    Builder b;
    REQUIRE(b.begin(info, sink) == Status::Ok);
    REQUIRE(b.finish() == Status::Ok);
    Reader r;
    CHECK(r.open(sink.bytes) == Status::Ok);
    CHECK(r.tiles() == 0);
}

// ── Golden files ─────────────────────────────────────────────────────────────
//
// Small synthetic files, kept in the repository with the CRC-32C of their
// decoded heights. Set PYRE_WRITE_GOLDEN=1 to write them again (only when the
// format changes on purpose).

struct Golden {
    const char* name;
    Params params;
    std::int32_t d;
    bool holes;
};

const Golden kGolden[] = {
    {"g40_b20_d99", Params{40, 20, 8, 1}, 99, false},
    {"g40_b20_d0_holes", Params{40, 20, 8, 1}, 0, true},
    {"g64_b16_g16_l2_d25_holes", Params{64, 16, 16, 2}, 25, true},
};

std::uint32_t decodedChecksum(const Reader& r) {
    std::uint32_t crc = 0;
    Scratch s;
    for (std::size_t k = 0; k < r.tiles(); ++k) {
        std::vector<std::int32_t> out(r.info().params.nodes());
        std::vector<std::uint8_t> mask(r.info().params.nodes());
        if (r.decodeWindow(k, 0, 0, r.info().params.tile, r.info().params.tile, 1, out, mask, s) != Status::Ok) return 0;
        crc = crc32c(out.data(), out.size() * 4, crc);
        if (r.spec(k).holes) crc = crc32c(mask.data(), mask.size(), crc);
    }
    return crc;
}

TEST(golden_files) {
    const bool write = std::getenv("PYRE_WRITE_GOLDEN") != nullptr;
    for (const Golden& g : kGolden) {
        const Built b = buildFile(g.params, Method::v1(), g.d, g.holes);
        const std::string path = std::string(PYRE_GOLDEN_DIR) + "/" + g.name + ".pyre";
        const std::string sumPath = std::string(PYRE_GOLDEN_DIR) + "/" + g.name + ".crc";
        Reader r;
        REQUIRE(r.open(b.file) == Status::Ok);
        const std::uint32_t sum = decodedChecksum(r);
        if (write) {
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.file.data()), std::streamsize(b.file.size()));
            char text[16];
            std::snprintf(text, sizeof text, "%08x\n", sum);
            std::ofstream(sumPath) << text;
            std::printf("  wrote %s (%zu bytes, decoded crc %08x)\n", path.c_str(), b.file.size(), sum);
            continue;
        }
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::vector<std::uint8_t> stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ifstream sumIn(sumPath);
        unsigned want = 0;
        sumIn >> std::hex >> want;
        // The stored file decodes to the stored checksum on this machine…
        Reader gr;
        REQUIRE(gr.open(stored) == Status::Ok);
        CHECK(decodedChecksum(gr) == want);
        // …and the encoder writes the same bytes.
        CHECK(stored == b.file);
    }
}

}  // namespace

CHECK_MAIN
