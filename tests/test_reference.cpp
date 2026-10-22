// pyre with Method::reference() must write, for a tile with no holes, the
// same streams as the reference (mundosm's heightmap.hpp, format 3), byte for
// byte, and reconstruct the same heights. This pins the port to the original.
// It builds only when the reference header is found (PYRE_REFERENCE_DIR).

#include <cstring>

#include "check.hpp"
#include "heightmap.hpp"   // mundosm::heightmap, read only
#include "pyre/tile.hpp"
#include "synth.hpp"

namespace ref = mundosm::heightmap;

namespace {

void compare(synth::Kind kind, int d, std::uint64_t seed) {
    const pyre::Params p{1000, 200, 8, 1};
    const auto x = synth::terrain(kind, ref::kSide, seed);
    std::vector<std::int32_t> refRecon(x.size()), recon(x.size());
    const std::vector<std::uint8_t> r = ref::encode(x.data(), d, refRecon.data());

    pyre::EncodedTile t;
    REQUIRE(pyre::encodeTile(p, pyre::Method::reference(), d, x, {}, t, recon) == pyre::Status::Ok);
    CHECK(recon == refRecon);

    // The reference tile: [u32 head length][head][25 × u32 block length][blocks].
    std::uint32_t headLen;
    std::memcpy(&headLen, r.data(), 4);
    REQUIRE(headLen == t.head.size());
    CHECK(std::memcmp(r.data() + 4, t.head.data(), headLen) == 0);
    std::size_t at = 4 + headLen + 4 * 25;
    for (int b = 0; b < 25; ++b) {
        std::uint32_t len;
        std::memcpy(&len, r.data() + 4 + headLen + 4 * b, 4);
        REQUIRE(len == t.blocks[std::size_t(b)].size());
        CHECK(std::memcmp(r.data() + at, t.blocks[std::size_t(b)].data(), len) == 0);
        at += len;
    }
    CHECK(at == r.size());

    // And pyre decodes the reference's streams.
    std::vector<std::int32_t> out(x.size());
    std::vector<std::uint8_t> none;
    pyre::Scratch s;
    const pyre::TileSpec spec{p, pyre::Method::reference(), d, false};
    REQUIRE(pyre::decodeHead(spec, {r.data() + 4, headLen}, out, none, 1, s) == pyre::Status::Ok);
    at = 4 + headLen + 4 * 25;
    for (std::uint32_t b = 0; b < 25; ++b) {
        std::uint32_t len;
        std::memcpy(&len, r.data() + 4 + headLen + 4 * b, 4);
        REQUIRE(pyre::decodeBlock(spec, {r.data() + at, len}, out, none, b % 5, b / 5, 1, s) == pyre::Status::Ok);
        at += len;
    }
    CHECK(out == refRecon);
}

TEST(reference_streams_are_identical) {
    std::uint64_t seed = 100;
    for (int d : {0, 1, 10, 99, 200})
        for (auto kind : {synth::Kind::Smooth, synth::Kind::Rough, synth::Kind::Constant, synth::Kind::Mountain, synth::Kind::Sea})
            compare(kind, d, ++seed);
}

}  // namespace

CHECK_MAIN
