// Unit and property tests of the codec: the coder, the model, the quantiser,
// the walk and one tile.

#include <algorithm>
#include <numeric>
#include <cstring>

#include "check.hpp"
#include "pyre/tile.hpp"
#include "synth.hpp"

using namespace pyre;

namespace {

std::vector<Params> someParams() {
    return {
        {1000, 200, 8, 1}, {1000, 100, 8, 1}, {1000, 500, 8, 1}, {1000, 100, 8, 2}, {1000, 1000, 8, 3},
        {64, 16, 16, 4},   {64, 32, 4, 2},    {2, 2, 2, 1},      {4, 2, 2, 1},      {96, 24, 32, 3},
        {256, 64, 32, 1},  {120, 40, 8, 2},
    };
}

// ── The coder ────────────────────────────────────────────────────────────────

void coderRoundtrip(Framing f, int bias, int n) {
    synth::Rng r(std::uint64_t(bias * 1000 + n));
    std::vector<bool> bits;
    std::vector<std::uint32_t> raws;
    for (int k = 0; k < n; ++k) {
        bits.push_back(r.range(0, 99) < bias);
        raws.push_back(std::uint32_t(r.next() & 0x1FFF));
    }
    std::vector<std::uint8_t> out;
    {
        Encoder e(out, f);
        std::uint16_t p[4] = {kProbInit, kProbInit, kProbInit, kProbInit};
        for (int k = 0; k < n; ++k) {
            e.bit(p[k % 4], bits[std::size_t(k)]);
            if (k % 17 == 0) e.raw(raws[std::size_t(k)], 13);
        }
        e.finish();
    }
    Decoder d(out.data(), out.size(), f);
    std::uint16_t p[4] = {kProbInit, kProbInit, kProbInit, kProbInit};
    for (int k = 0; k < n; ++k) {
        CHECK(d.bit(p[k % 4]) == bits[std::size_t(k)]);
        if (k % 17 == 0) CHECK(d.raw(13) == raws[std::size_t(k)]);
    }
    if (f == Framing::Lzma) {
        CHECK(out.size() >= 1 && out[0] == 0);
        CHECK(d.pastEnd() == 0);
    }
}

TEST(coder_roundtrips) {
    for (Framing f : {Framing::Lzma, Framing::Trimmed})
        for (int bias : {0, 1, 10, 50, 90, 99, 100})
            for (int n : {0, 1, 2, 7, 100, 10000}) coderRoundtrip(f, bias, n);
}

TEST(coder_trimmed_is_shorter) {
    std::vector<std::uint8_t> a, b;
    for (auto [out, f] : {std::pair{&a, Framing::Lzma}, std::pair{&b, Framing::Trimmed}}) {
        Encoder e(*out, f);
        std::uint16_t p = kProbInit;
        for (int k = 0; k < 1000; ++k) e.bit(p, k % 7 == 0);
        e.finish();
    }
    CHECK(b.size() + 4 <= a.size());
    // A stream of sure zeros trims to nearly nothing.
    std::vector<std::uint8_t> z;
    Encoder e(z, Framing::Trimmed);
    std::uint16_t p = kProbInit;
    for (int k = 0; k < 100; ++k) e.bit(p, false);
    e.finish();
    CHECK(z.size() <= 3);
}

// ── The model ────────────────────────────────────────────────────────────────

TEST(model_zigzag) {
    for (std::int64_t q : {-5ll, -1ll, 0ll, 1ll, 2ll, 1000ll, -1000ll, (1ll << 31) - 1, -(1ll << 31)})
        CHECK(unzigzag(zigzag(q)) == q);
    CHECK(zigzag(0) == 0 && zigzag(-1) == 1 && zigzag(1) == 2);
}

TEST(model_values_roundtrip) {
    std::vector<std::int64_t> qs;
    for (int q = -40; q < 40; ++q) qs.push_back(q);
    for (std::int64_t q : {1ll << 20, -(1ll << 20), (1ll << 31) - 1, -(1ll << 31), 12345ll, -7ll, (1ll << 30), -(1ll << 30) - 5})
        qs.push_back(q);
    for (Framing f : {Framing::Lzma, Framing::Trimmed}) {
        std::vector<std::uint8_t> out;
        Encoder e(out, f);
        Model m;
        m.reset();
        for (std::size_t k = 0; k < qs.size(); ++k) write(e, m.c[k % 3], qs[k]);
        e.finish();
        Decoder d(out.data(), out.size(), f);
        m.reset();
        for (std::size_t k = 0; k < qs.size(); ++k) CHECK(read(d, m.c[k % 3]) == qs[k]);
    }
}

TEST(model_bands) {
    CHECK(band(0, 99) == 0 && band(98, 99) == 0 && band(99, 99) == 1 && band(197, 99) == 1 && band(198, 99) == 2);
    CHECK(band(INT64_MAX / 4, 99) == kBands - 1);
    CHECK(band(0, 0) == 0 && band(1, 0) == 1);
}

TEST(model_garbage_is_safe) {
    std::vector<std::uint8_t> data(4096);
    for (std::size_t k = 0; k < data.size(); ++k) data[k] = std::uint8_t((k * 2654435761u) >> 13);
    for (Framing f : {Framing::Lzma, Framing::Trimmed}) {
        Decoder d(data.data(), data.size(), f);
        Model m;
        m.reset();
        for (int k = 0; k < 10000; ++k) read(d, m.c[0]);
        std::vector<std::uint8_t> ones(64, 0xFF);
        Decoder d2(ones.data(), ones.size(), f);
        for (int k = 0; k < 1000; ++k) read(d2, m.c[1]);
        Decoder d3(nullptr, 0, f);
        for (int k = 0; k < 1000; ++k) read(d3, m.c[2]);
    }
}

// ── The quantiser ────────────────────────────────────────────────────────────

TEST(quantizer_bound) {
    for (int d : {0, 1, 2, 5, 99, 100000})
        for (std::int64_t pred : {-1000ll, -3ll, 0ll, 7ll, 12345ll})
            for (std::int64_t x = -400; x < 400; ++x) {
                const std::int64_t r = pred + quantize(x, pred, d) * (2 * std::int64_t(d) + 1);
                CHECK(std::abs(r - x) <= d);
            }
    CHECK(quantize(5, 0, 0) == 5 && quantize(-5, 0, 0) == -5);
    CHECK(quantize(99, 0, 99) == 0 && quantize(100, 0, 99) == 1 && quantize(-99, 0, 99) == 0 && quantize(-100, 0, 99) == -1);
}

// ── The walk ─────────────────────────────────────────────────────────────────

struct Counter {
    static constexpr bool kHoles = false;
    std::vector<int>& seen;
    bool corner(std::size_t k, std::int32_t& v) {
        ++seen[k];
        v = 0;
        return true;
    }
    bool node(std::size_t k, std::int64_t, int ctx, int, std::int32_t& v) {
        ++seen[k];
        CHECK(ctx >= 0 && ctx < kContexts);
        v = 0;
        return true;
    }
};

TEST(walk_every_node_once) {
    for (const Params& p : someParams()) {
        REQUIRE(p.valid());
        const detail::Geo geo(p);
        std::vector<std::int32_t> r(p.nodes());
        std::vector<int> seen(p.nodes());
        Counter c{seen};
        detail::Grid g{r.data(), nullptr};
        detail::walkHead(g, geo, 0, c, 1);
        for (std::size_t by = 0; by < geo.kb; ++by)
            for (std::size_t bx = 0; bx < geo.kb; ++bx) detail::walkBlock(g, geo, 0, c, bx, by, 1);
        CHECK(std::all_of(seen.begin(), seen.end(), [](int s) { return s == 1; }));
    }
}

TEST(walk_head_levels) {
    for (const Params& p : someParams()) {
        const detail::Geo geo(p);
        const std::size_t n = geo.n, hs = p.headStep();
        for (std::size_t step = p.base; step >= 1; step /= 2) {
            std::vector<std::int32_t> r(p.nodes());
            std::vector<int> seen(p.nodes());
            Counter c{seen};
            detail::Grid g{r.data(), nullptr};
            detail::walkHead(g, geo, 0, c, step);
            int wrong = 0;
            for (std::size_t j = 0; j < n; ++j)
                for (std::size_t i = 0; i < n; ++i) {
                    const bool side = i == 0 || j == 0 || i == geo.u || j == geo.u;
                    const bool onGrid = i % step == 0 && j % step == 0;
                    const bool headLevel = i % hs == 0 && j % hs == 0;
                    const bool onLine = (i % geo.b == 0 && j % step == 0) || (j % geo.b == 0 && i % step == 0);
                    wrong += (seen[j * n + i] == 1) != (side || (onGrid && (headLevel || onLine)));
                }
            CHECK(wrong == 0);
        }
    }
}

/// A block reads only its own nodes and the head's.
struct Poisoned {
    static constexpr bool kHoles = false;
    bool corner(std::size_t, std::int32_t& v) {
        v = 0;
        return true;
    }
    bool node(std::size_t, std::int64_t pred, int, int, std::int32_t& v) {
        CHECK(std::abs(pred) < (1 << 20));
        v = 0;
        return true;
    }
};

TEST(walk_blocks_are_independent) {
    for (const Params& p : someParams()) {
        const detail::Geo geo(p);
        std::vector<std::int32_t> r(p.nodes());
        std::vector<int> seen(p.nodes());
        Counter c{seen};
        detail::Grid g{r.data(), nullptr};
        detail::walkHead(g, geo, 0, c, 1);
        for (std::size_t by = 0; by < geo.kb; ++by)
            for (std::size_t bx = 0; bx < geo.kb; ++bx) {
                for (std::size_t k = 0; k < r.size(); ++k) r[k] = seen[k] ? 0 : (1 << 28);
                Poisoned v;
                detail::walkBlock(g, geo, 0, v, bx, by, 1);
                const std::size_t x0 = bx * geo.b, y0 = by * geo.b;
                for (std::size_t j = y0; j <= y0 + geo.b; ++j)
                    for (std::size_t i = x0; i <= x0 + geo.b; ++i) CHECK(r[j * geo.n + i] == 0);
            }
    }
}

// ── One tile ─────────────────────────────────────────────────────────────────

struct Decoded {
    std::vector<std::int32_t> h;
    std::vector<std::uint8_t> m;
};

Decoded decodeAll(const TileSpec& spec, const EncodedTile& t, std::uint32_t minStep = 1) {
    Decoded out{std::vector<std::int32_t>(spec.params.nodes(), 0), std::vector<std::uint8_t>(spec.holes ? spec.params.nodes() : 0)};
    Scratch s;
    REQUIRE(decodeHead(spec, t.head, out.h, out.m, minStep, s) == Status::Ok);
    const std::uint32_t kb = spec.params.blocksPerSide();
    for (std::uint32_t by = 0; by < kb; ++by)
        for (std::uint32_t bx = 0; bx < kb; ++bx)
            REQUIRE(decodeBlock(spec, t.blocks[by * kb + bx], out.h, out.m, bx, by, minStep, s) == Status::Ok);
    return out;
}

void roundtrip(const Params& p, Method method, int d, synth::Kind kind, std::uint64_t seed, bool holes) {
    const auto x = synth::terrain(kind, int(p.side()), seed);
    const auto mask = holes ? synth::holes(int(p.side()), seed, int(p.nodes() / 50) + 1, true) : std::vector<std::uint8_t>{};
    EncodedTile t;
    std::vector<std::int32_t> recon(p.nodes());
    const Status st = encodeTile(p, method, d, x, mask, t, recon);
    if (st == Status::Empty) return;
    REQUIRE(st == Status::Ok);
    const TileSpec spec{p, method, d, t.holes};
    const Decoded out = decodeAll(spec, t);
    CHECK(out.h == recon);
    std::int64_t worst = 0;
    for (std::size_t k = 0; k < p.nodes(); ++k) {
        const bool valid = mask.empty() || mask[k];
        if (t.holes) CHECK(out.m[k] == mask[k]);
        if (valid) worst = std::max<std::int64_t>(worst, std::abs(std::int64_t(out.h[k]) - x[k]));
    }
    CHECK(worst <= d);
    if (d == 0) CHECK(worst == 0);
}

TEST(tile_roundtrip_and_error_limit) {
    std::uint64_t seed = 1;
    for (const Params& p : someParams())
        for (Method m : {Method::reference(), Method::v1()})
            for (int d : {0, 1, 10, 50, 99, 200})
                for (auto kind : {synth::Kind::Smooth, synth::Kind::Rough, synth::Kind::Constant, synth::Kind::Mountain, synth::Kind::Sea})
                    for (bool holes : {false, true}) {
                        if (p.tile >= 1000 && (d % 99 != 0 || kind == synth::Kind::Constant)) continue;   // keep it quick
                        roundtrip(p, m, d, kind, ++seed, holes);
                    }
}

TEST(tile_random_parameters) {
    synth::Rng r(77);
    int done = 0;
    while (done < 150) {
        Params p;
        p.base = 1u << r.range(1, 6);
        p.blockLevels = std::uint32_t(r.range(1, std::countr_zero(p.base)));
        const std::uint32_t hs = 1u << p.blockLevels;
        p.block = hs * std::uint32_t(r.range(1, 6));
        const std::uint32_t lcm = std::lcm(p.block, p.base);
        p.tile = lcm * std::uint32_t(r.range(1, std::max(1, int(300 / lcm))));
        if (!p.valid()) continue;
        const auto kind = synth::Kind(r.range(0, 4));
        roundtrip(p, Method::v1(), r.range(0, 3) == 0 ? 0 : r.range(0, 300), kind, r.next(), r.range(0, 1) == 1);
        ++done;
    }
}

TEST(tile_all_holes_is_empty) {
    const Params p{16, 8, 4, 1};
    std::vector<std::int32_t> x(p.nodes(), 5);
    std::vector<std::uint8_t> m(p.nodes(), 0);
    EncodedTile t;
    CHECK(encodeTile(p, Method::v1(), 10, x, m, t) == Status::Empty);
    m[17] = 1;   // one valid node is enough
    CHECK(encodeTile(p, Method::v1(), 0, x, m, t) == Status::Ok);
    CHECK(t.holes && t.valid == 1 && t.min == 5 && t.max == 5);
}

TEST(tile_rejects_bad_input) {
    const Params p{16, 8, 4, 1};
    std::vector<std::int32_t> x(p.nodes(), 5);
    EncodedTile t;
    CHECK(encodeTile(p, Method::v1(), -1, x, {}, t) == Status::BadInput);
    CHECK(encodeTile(p, Method::v1(), kMaxError + 1, x, {}, t) == Status::BadInput);
    CHECK(encodeTile(Params{16, 6, 4, 1}, Method::v1(), 1, x, {}, t) == Status::BadParams);
    x[3] = kMaxHeight + 1;
    CHECK(encodeTile(p, Method::v1(), 1, x, {}, t) == Status::BadInput);
    std::vector<std::int32_t> small(10);
    CHECK(encodeTile(p, Method::v1(), 1, small, {}, t) == Status::Usage);
}

/// A block decoded alone (after the head) equals the same block from a full decode.
TEST(tile_random_access) {
    for (const Params& p : {Params{1000, 200, 8, 1}, Params{1000, 100, 8, 2}, Params{120, 40, 8, 3}})
        for (bool holes : {false, true}) {
            const auto x = synth::terrain(synth::Kind::Mountain, int(p.side()), 5);
            const auto mask = holes ? synth::holes(int(p.side()), 9, 3000, true) : std::vector<std::uint8_t>{};
            EncodedTile t;
            REQUIRE(encodeTile(p, Method::v1(), 99, x, mask, t) == Status::Ok);
            const TileSpec spec{p, Method::v1(), 99, t.holes};
            const Decoded full = decodeAll(spec, t);
            const std::uint32_t kb = p.blocksPerSide();
            Scratch s;
            for (std::uint32_t by = 0; by < kb; by += 2)
                for (std::uint32_t bx = kb - 1; bx < kb; --bx) {
                    Decoded one{std::vector<std::int32_t>(p.nodes(), INT32_MIN), std::vector<std::uint8_t>(holes ? p.nodes() : 0, 7)};
                    REQUIRE(decodeHead(spec, t.head, one.h, one.m, 1, s) == Status::Ok);
                    REQUIRE(decodeBlock(spec, t.blocks[by * kb + bx], one.h, one.m, bx, by, 1, s) == Status::Ok);
                    for (std::uint32_t j = by * p.block; j <= (by + 1) * p.block; ++j)
                        for (std::uint32_t i = bx * p.block; i <= (bx + 1) * p.block; ++i) {
                            const std::size_t k = std::size_t(j) * p.side() + i;
                            CHECK(one.h[k] == full.h[k]);
                            if (holes) CHECK(one.m[k] == full.m[k]);
                        }
                }
        }
}

/// A decode stopped at a level equals the full decode at that level's nodes.
TEST(tile_levels) {
    for (const Params& p : {Params{1000, 200, 8, 1}, Params{1000, 100, 8, 2}, Params{256, 64, 32, 3}})
        for (bool holes : {false, true}) {
            const auto x = synth::terrain(synth::Kind::Rough, int(p.side()), 11);
            const auto mask = holes ? synth::holes(int(p.side()), 12, 500, true) : std::vector<std::uint8_t>{};
            EncodedTile t;
            REQUIRE(encodeTile(p, Method::v1(), 50, x, mask, t) == Status::Ok);
            const TileSpec spec{p, Method::v1(), 50, t.holes};
            const Decoded full = decodeAll(spec, t);
            for (std::uint32_t step = p.base; step >= 1; step /= 2) {
                const Decoded part = decodeAll(spec, t, step);
                for (std::size_t j = 0; j < p.side(); j += step)
                    for (std::size_t i = 0; i < p.side(); i += step) {
                        const std::size_t k = j * p.side() + i;
                        CHECK(part.h[k] == full.h[k]);
                        if (holes) CHECK(part.m[k] == full.m[k]);
                    }
            }
        }
}

/// Two neighbour tiles decode the same values on their shared side.
TEST(tile_no_cracks) {
    const Params p{1000, 200, 8, 1};
    const int side = int(p.side());
    for (int d : {0, 99, 200})
        for (bool holes : {false, true}) {
            // Tiles (0,0), (1,0), (0,1) of one global terrain.
            auto encodeAt = [&](int ti, int tj) {
                const auto x = synth::terrain(synth::Kind::Rough, side, 3, ti, tj);
                std::vector<std::uint8_t> m;
                if (holes) {
                    // Holes from a global hash, so both tiles agree on the shared side.
                    m.assign(p.nodes(), 1);
                    for (int j = 0; j < side; ++j)
                        for (int i = 0; i < side; ++i) {
                            const std::uint64_t gx = std::uint64_t(ti * (side - 1) + i), gy = std::uint64_t(tj * (side - 1) + j);
                            if (((gx * 31 + gy * 17) ^ (gx * gy)) % 23 == 0) m[std::size_t(j) * side + i] = 0;
                        }
                }
                EncodedTile t;
                REQUIRE(encodeTile(p, Method::v1(), d, x, m, t) == Status::Ok);
                return decodeAll(TileSpec{p, Method::v1(), d, t.holes}, t);
            };
            const Decoded a = encodeAt(0, 0), east = encodeAt(1, 0), north = encodeAt(0, 1);
            for (int k = 0; k < side; ++k) {
                CHECK(a.h[std::size_t(k) * side + 1000] == east.h[std::size_t(k) * side + 0]);
                CHECK(a.h[std::size_t(1000) * side + k] == north.h[std::size_t(k)]);
            }
        }
}

/// Damaged streams give some heights or an error, never a crash. (The file's
/// checksums find the damage; here only safety is checked. Run it under ASan
/// and UBSan.)
TEST(tile_damage_is_safe) {
    const Params p{120, 40, 8, 1};
    const auto x = synth::terrain(synth::Kind::Mountain, int(p.side()), 21);
    const auto mask = synth::holes(int(p.side()), 22, 100, true);
    synth::Rng r(5);
    for (bool holes : {false, true})
        for (Method m : {Method::reference(), Method::v1()}) {
            EncodedTile t;
            REQUIRE(encodeTile(p, m, 99, x, holes ? std::span<const std::uint8_t>(mask) : std::span<const std::uint8_t>{}, t) == Status::Ok);
            const TileSpec spec{p, m, 99, t.holes};
            Decoded out{std::vector<std::int32_t>(p.nodes()), std::vector<std::uint8_t>(p.nodes())};
            Scratch s;
            for (int trial = 0; trial < 300; ++trial) {
                std::vector<std::uint8_t> bad = t.head;
                switch (trial % 3) {
                    case 0:
                        for (auto& b : bad) b = std::uint8_t(r.next());
                        break;
                    case 1: bad.resize(bad.size() * std::size_t(r.range(0, 99)) / 100); break;
                    case 2:
                        if (!bad.empty()) bad[std::size_t(r.range(0, int(bad.size()) - 1))] ^= std::uint8_t(1u << r.range(0, 7));
                        break;
                }
                const Status st = decodeHead(spec, bad, out.h, out.m, 1, s);
                CHECK(st == Status::Ok || st == Status::Corrupt);
                for (std::uint32_t b = 0; b < p.blocks(); ++b) {
                    const Status sb = decodeBlock(spec, bad, out.h, out.m, b % 3, b / 3, 1, s);
                    CHECK(sb == Status::Ok || sb == Status::Corrupt);
                }
            }
        }
}

TEST(tile_bad_calls) {
    const Params p{16, 8, 4, 1};
    std::vector<std::int32_t> x(p.nodes(), 5), out(p.nodes());
    std::vector<std::uint8_t> m;
    EncodedTile t;
    REQUIRE(encodeTile(p, Method::v1(), 1, x, {}, t) == Status::Ok);
    const TileSpec spec{p, Method::v1(), 1, false};
    Scratch s;
    CHECK(decodeHead(spec, t.head, out, m, 3, s) == Status::Usage);
    CHECK(decodeHead(spec, t.head, out, m, 8, s) == Status::Usage);
    CHECK(decodeHead(spec, t.head, std::span<std::int32_t>(out).first(10), m, 1, s) == Status::Usage);
    CHECK(decodeBlock(spec, t.blocks[0], out, m, 2, 0, 1, s) == Status::Usage);
    TileSpec holes = spec;
    holes.holes = true;
    CHECK(decodeHead(holes, t.head, out, m, 1, s) == Status::Usage);
}

TEST(window_blocks) {
    const Params p;
    auto eq = [](BlockRange r, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        return !r.empty && r.bx0 == a && r.by0 == b && r.bx1 == c && r.by1 == d;
    };
    CHECK(eq(blocksOfWindow(p, 0, 0, 1000, 1000), 0, 0, 4, 4));
    CHECK(eq(blocksOfWindow(p, 0, 0, 200, 200), 0, 0, 0, 0));
    CHECK(eq(blocksOfWindow(p, 0, 0, 201, 200), 0, 0, 1, 0));
    CHECK(eq(blocksOfWindow(p, 399, 0, 401, 5), 1, 0, 2, 0));
    CHECK(blocksOfWindow(p, 200, 0, 200, 1000).empty);
    CHECK(blocksOfWindow(p, 1000, 1000, 1000, 1000).empty);
    CHECK(blocksOfWindow(p, 0, 0, 0, 0).empty);
}

}  // namespace

CHECK_MAIN
