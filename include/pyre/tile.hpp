#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  tile.hpp — one tile: encode it; decode its head, one block, or a window
// ═════════════════════════════════════════════════════════════════════════════
//
// A tile is (S + 1)² heights in quanta, row-major: row j (northwards) major,
// column i (eastwards) minor. The head is one coder stream; each block is
// another, with its own model, so a block decodes with the head alone.
//
// Holes (no-data). A tile with holes codes a validity bit for each node, in
// the walk's order, with a context from how many of the node's predictors are
// valid (a binary context model, as JBIG's progressive mode). A hole costs
// its validity bit and nothing else: its height is its prediction, so the
// pyramid stays whole. Whether a tile has holes is a flag in the file's index
// (one bit a tile), so a tile with no holes codes no validity bits at all.
// A corner under a hole is 0: the value must be the same in the four tiles
// that share the corner, and 0 depends on nothing.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "model.hpp"
#include "params.hpp"
#include "rc.hpp"
#include "walk.hpp"

namespace pyre {

/// The coding method. The file stores it as bits, so a reader never guesses.
struct Method {
    Framing framing = Framing::Trimmed;

    /// The reference's method: for a tile with no holes, its streams are byte
    /// for byte those of the reference heightmap.hpp (format 3).
    static constexpr Method reference() { return Method{Framing::Lzma}; }
    /// pyre v0.1.
    static constexpr Method v1() { return Method{Framing::Trimmed}; }

    constexpr std::uint32_t bits() const { return std::uint32_t(framing); }
    /// False for bits that this reader does not know.
    static constexpr bool fromBits(std::uint32_t bits, Method& m) {
        if (bits & ~1u) return false;
        m.framing = Framing(bits & 1u);
        return true;
    }
    friend constexpr bool operator==(const Method&, const Method&) = default;
};

/// What the decoder of one tile needs: all of it is in the file's header and index.
struct TileSpec {
    Params params;
    Method method;
    std::int32_t d = 0;   ///< D: the error limit, in quanta
    bool holes = false;   ///< some nodes are not valid
};

struct EncodedTile {
    std::vector<std::uint8_t> head;
    std::vector<std::vector<std::uint8_t>> blocks;   ///< rows south to north, each west to east
    std::int32_t min = 0, max = 0;                    ///< of the decoded valid heights
    bool holes = false;
    std::uint64_t valid = 0;                          ///< the count of valid nodes

    std::size_t bytes() const {
        std::size_t n = head.size();
        for (const auto& b : blocks) n += b.size();
        return n;
    }
};

/// The buffer that a decoder reuses: the model (about 32 KB). One for each
/// thread. With it, decoding allocates nothing.
struct Scratch {
    std::unique_ptr<Model> model = std::make_unique<Model>();
};

/// JPEG-LS's near-lossless quantised difference: |x − (pred + q·(2d + 1))| ≤ d.
inline std::int64_t quantize(std::int64_t x, std::int64_t pred, std::int32_t d) {
    const std::int64_t r = x - pred, step = 2 * std::int64_t(d) + 1;
    return r >= 0 ? (r + d) / step : -((d - r) / step);
}

namespace detail {

template <bool H>
struct EncodeVisitor {
    static constexpr bool kHoles = H;
    Encoder& e;
    Model& m;
    const std::int32_t* x;
    const std::uint8_t* valid;
    std::int32_t d;
    std::int64_t step;

    bool corner(std::size_t k, std::int32_t& out) {
        if constexpr (H) {
            const bool ok = valid[k] != 0;
            e.bit(m.mask[0], ok);
            if (!ok) { out = 0; return false; }
        }
        e.raw(std::uint32_t(x[k]) ^ 0x80000000u, 32);
        out = x[k];
        return true;
    }
    bool node(std::size_t k, std::int64_t pred, int ctx, int mctx, std::int32_t& out) {
        if constexpr (H) {
            const bool ok = valid[k] != 0;
            e.bit(m.mask[std::size_t(mctx)], ok);
            if (!ok) { out = std::int32_t(pred); return false; }
        }
        const std::int64_t q = quantize(x[k], pred, d);
        write(e, m.c[std::size_t(ctx)], q);
        out = std::int32_t(pred + q * step);
        return true;
    }
};

template <bool H>
struct DecodeVisitor {
    static constexpr bool kHoles = H;
    Decoder& dec;
    Model& m;
    std::int64_t step;

    bool corner(std::size_t, std::int32_t& out) {
        if constexpr (H) {
            if (!dec.bit(m.mask[0])) { out = 0; return false; }
        }
        out = std::int32_t(dec.raw(32) ^ 0x80000000u);
        return true;
    }
    bool node(std::size_t, std::int64_t pred, int ctx, int mctx, std::int32_t& out) {
        if constexpr (H) {
            if (!dec.bit(m.mask[std::size_t(mctx)])) { out = std::int32_t(pred); return false; }
        }
        // |q| ≤ 2^31 and step ≤ 2^30 + 1: the product fits int64 with room. The
        // cast to int32 keeps the low bits (defined since C++20).
        out = std::int32_t(pred + read(dec, m.c[std::size_t(ctx)]) * step);
        return true;
    }
};

/// Which stream a walk codes.
struct Part {
    bool head = true;
    std::size_t bx = 0, by = 0, minStep = 1;
};

template <class V>
void run(Grid& g, const Geo& geo, int d, V& v, const Part& part) {
    if (part.head) walkHead(g, geo, d, v, part.minStep);
    else walkBlock(g, geo, d, v, part.bx, part.by, part.minStep);
}

inline Status decodePart(const TileSpec& spec, std::span<const std::uint8_t> data, std::span<std::int32_t> out,
                         std::span<std::uint8_t> mask, const Part& part, Scratch& s) {
    if (!spec.params.valid()) return Status::BadParams;
    if (out.size() != spec.params.nodes() || (spec.holes && mask.size() != spec.params.nodes())) return Status::Usage;
    if (spec.d < 0 || spec.d > kMaxError) return Status::Corrupt;
    const Geo geo(spec.params);
    Grid g{out.data(), spec.holes ? mask.data() : nullptr};
    Model& m = *s.model;
    m.reset();
    Decoder dec(data.data(), data.size(), spec.method.framing);
    const std::int64_t step = 2 * std::int64_t(spec.d) + 1;
    if (spec.holes) {
        DecodeVisitor<true> v{dec, m, step};
        run(g, geo, spec.d, v, part);
    } else {
        DecodeVisitor<false> v{dec, m, step};
        run(g, geo, spec.d, v, part);
    }
    // The LZMA framing reads its bytes exactly: more means a cut stream.
    // (Trimmed streams are checked by the file's checksums.)
    if (spec.method.framing == Framing::Lzma && dec.pastEnd() > 4) return Status::Corrupt;
    return Status::Ok;
}

inline bool stepValid(const TileSpec& spec, std::uint32_t minStep) {
    return minStep >= 1 && (minStep & (minStep - 1)) == 0 && minStep <= spec.params.base;
}

}  // namespace detail

/// Encodes one tile. `x`: the (S + 1)² heights in quanta. `valid`: 1 for a
/// valid node, 0 for a hole; empty if all are valid. Heights under holes are
/// not read. `recon`, if not empty, receives what the decoder will produce
/// (a hole holds its prediction).
inline Status encodeTile(const Params& p, Method method, std::int32_t d, std::span<const std::int32_t> x,
                         std::span<const std::uint8_t> valid, EncodedTile& out, std::span<std::int32_t> recon = {}) {
    if (!p.valid()) return Status::BadParams;
    const std::size_t nodes = p.nodes();
    if (x.size() != nodes || (!valid.empty() && valid.size() != nodes) || (!recon.empty() && recon.size() != nodes))
        return Status::Usage;
    if (d < 0 || d > kMaxError) return Status::BadInput;
    std::uint64_t count = 0;
    for (std::size_t k = 0; k < nodes; ++k) {
        if (!valid.empty() && valid[k] == 0) continue;
        ++count;
        if (x[k] > kMaxHeight || x[k] < -kMaxHeight) return Status::BadInput;
    }
    if (count == 0) return Status::Empty;
    const bool holes = count < nodes;

    std::vector<std::int32_t> own;
    if (recon.empty()) {
        own.resize(nodes);
        recon = own;
    }
    std::vector<std::uint8_t> mask(holes ? nodes : 0);
    const detail::Geo geo(p);
    detail::Grid g{recon.data(), holes ? mask.data() : nullptr};
    Scratch s;
    const std::int64_t step = 2 * std::int64_t(d) + 1;

    auto stream = [&](const detail::Part& part) {
        std::vector<std::uint8_t> bytes;
        Encoder e(bytes, method.framing);
        s.model->reset();
        if (holes) {
            detail::EncodeVisitor<true> v{e, *s.model, x.data(), valid.data(), d, step};
            detail::run(g, geo, d, v, part);
        } else {
            detail::EncodeVisitor<false> v{e, *s.model, x.data(), nullptr, d, step};
            detail::run(g, geo, d, v, part);
        }
        e.finish();
        return bytes;
    };

    out = EncodedTile{};
    out.holes = holes;
    out.valid = count;
    out.head = stream(detail::Part{true, 0, 0, 1});
    for (std::size_t by = 0; by < geo.kb; ++by)
        for (std::size_t bx = 0; bx < geo.kb; ++bx) out.blocks.push_back(stream(detail::Part{false, bx, by, 1}));
    std::int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (std::size_t k = 0; k < nodes; ++k)
        if (!holes || mask[k]) {
            lo = std::min(lo, recon[k]);
            hi = std::max(hi, recon[k]);
        }
    out.min = lo;
    out.max = hi;
    return Status::Ok;
}

/// Decodes the head down to the step minStep (1, 2, 4… up to G). Then the
/// nodes on that step's grid outside the blocks are right, and the sides are
/// whole; with a step finer than the head step, the block lines too. `mask`
/// receives the validity (1 or 0) when the tile has holes; with no holes it
/// is not touched (pass an empty span).
inline Status decodeHead(const TileSpec& spec, std::span<const std::uint8_t> head, std::span<std::int32_t> out,
                         std::span<std::uint8_t> mask, std::uint32_t minStep, Scratch& s) {
    if (!detail::stepValid(spec, minStep)) return Status::Usage;
    return detail::decodePart(spec, head, out, mask, detail::Part{true, 0, 0, minStep}, s);
}

/// Decodes block (bx, by) down to minStep into `out` and `mask`, which hold
/// the head decoded down to the same step or finer. It writes only the nodes
/// inside the block and keeps no state: threads may decode different blocks
/// of one tile into one buffer at once, each with its own Scratch.
inline Status decodeBlock(const TileSpec& spec, std::span<const std::uint8_t> block, std::span<std::int32_t> out,
                          std::span<std::uint8_t> mask, std::uint32_t bx, std::uint32_t by, std::uint32_t minStep,
                          Scratch& s) {
    if (!detail::stepValid(spec, minStep)) return Status::Usage;
    const std::uint32_t kb = spec.params.blocksPerSide();
    if (!spec.params.valid() || bx >= kb || by >= kb) return Status::Usage;
    return detail::decodePart(spec, block, out, mask, detail::Part{false, bx, by, minStep}, s);
}

/// The blocks that hold nodes of the window [i0, i1] × [j0, j1] (nodes, both
/// ends included), as inclusive ranges. A node on a block line is the head's.
struct BlockRange {
    std::uint32_t bx0, by0, bx1, by1;
    bool empty;
};
inline BlockRange blocksOfWindow(const Params& p, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1) {
    const std::uint32_t b = p.block, kb = p.blocksPerSide();
    // Block t holds the nodes t·B < i < (t+1)·B.
    auto first = [&](std::uint32_t v) { return v / b; };                  // the first block with an inside node ≥ v
    auto last = [&](std::uint32_t v) { return v == 0 ? 0 : (v - 1) / b; };   // the last with an inside node ≤ v
    BlockRange r{first(i0), first(j0), std::min(last(i1), kb - 1), std::min(last(j1), kb - 1), false};
    // A window inside one block line only (i0 = i1 = t·B) touches no block.
    auto none = [&](std::uint32_t a0, std::uint32_t a1) { return a0 > a1 || (a0 == a1 && a0 % b == 0); };
    r.empty = none(i0, i1) || none(j0, j1) || r.bx0 > r.bx1 || r.by0 > r.by1;
    return r;
}

}  // namespace pyre
