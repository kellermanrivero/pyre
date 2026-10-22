#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  model.hpp — a quantised difference in binary, with its context
// ═════════════════════════════════════════════════════════════════════════════
//
// The difference q becomes a natural number by zigzag (0, −1, 1, −2… → 0, 1, 2,
// 3…). Then: "is it zero?", unary up to 15 with a probability for each
// position, and the rest in Exp-Golomb (the length with a probability for each
// bit, the bits without one). For terrain nearly every q is 0 or ±1: the first
// two or three bits carry most of it.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "rc.hpp"

namespace pyre {

/// Bands of local activity.
inline constexpr int kBands = 12;
/// The most levels of a pyramid: the base and 8 refinements (G ≤ 256).
inline constexpr int kMaxLevels = 9;
/// 2D contexts: level × (centre, edge midpoint) × band.
inline constexpr int kContexts2D = kMaxLevels * 2 * kBands;
/// All contexts: the 2D ones, then the 1D ones (sides, block lines): level × band.
inline constexpr int kContexts = kContexts2D + kMaxLevels * kBands;
/// Validity contexts: the corners, then level × kind (1D, centre, edge) × the
/// count of valid predictors (0 to 4).
inline constexpr int kMaskContexts = 1 + kMaxLevels * 3 * 5;

struct Context {
    std::uint16_t zero;
    std::array<std::uint16_t, 15> unary;
    std::array<std::uint16_t, 32> length;
};

/// All probabilities of one stream: about 32 KB. A decoder keeps one in a
/// `Scratch` and reuses it, so decoding a block allocates nothing.
struct Model {
    std::array<Context, kContexts> c;
    std::array<std::uint16_t, kMaskContexts> mask;

    void reset() {
        Context fresh;
        fresh.zero = kProbInit;
        fresh.unary.fill(kProbInit);
        fresh.length.fill(kProbInit);
        c.fill(fresh);
        mask.fill(kProbInit);
    }
};

inline constexpr int ctx2D(int level, int kind, int band) { return (level * 2 + kind) * kBands + band; }
inline constexpr int ctx1D(int level, int band) { return kContexts2D + level * kBands + band; }

inline constexpr int kMask1D = 0, kMaskCentre = 1, kMaskEdge = 2;
inline constexpr int maskCtx(int level, int kind, int valid) { return 1 + (level * 3 + kind) * 5 + valid; }

inline std::uint32_t zigzag(std::int64_t q) { return q >= 0 ? std::uint32_t(2 * q) : std::uint32_t(-2 * q - 1); }
inline std::int64_t unzigzag(std::uint32_t z) { return (z & 1u) ? -std::int64_t(z >> 1) - 1 : std::int64_t(z >> 1); }

/// Writes q, |q| ≤ 2^31.
inline void write(Encoder& e, Context& c, std::int64_t q) {
    const std::uint32_t z = zigzag(q);
    e.bit(c.zero, z != 0);
    if (z == 0) return;
    const std::uint32_t v = z - 1;
    for (std::uint32_t k = 0; k < 15; ++k) {
        e.bit(c.unary[k], v > k);
        if (v <= k) return;
    }
    // Order-0 Exp-Golomb of v − 15: n ones in unary with a context, a zero, n raw bits.
    const std::uint32_t w = v - 15 + 1;
    const int n = 31 - std::countl_zero(w);
    for (int k = 0; k < n; ++k) e.bit(c.length[std::size_t(k)], true);
    // The decoder stops at 31 ones with no zero after them. (The reference
    // writes the zero there too and gets out of step, but only for |q| ≥ 2^30,
    // which centimetre heights never give: the streams stay the same.)
    if (n < 31) e.bit(c.length[std::size_t(n)], false);
    e.raw(w & ((1u << n) - 1u), n);
}

/// Reads a q. A damaged stream gives some value, never undefined behaviour.
inline std::int64_t read(Decoder& d, Context& c) {
    if (!d.bit(c.zero)) return 0;
    std::uint32_t v = 0;
    while (v < 15 && d.bit(c.unary[v])) ++v;
    if (v == 15) {
        int n = 0;
        while (n < 31 && d.bit(c.length[std::size_t(n)])) ++n;
        const std::uint32_t w = (1u << n) | d.raw(n);
        v = w - 1u + 15u;   // unsigned: wraps, never undefined
    }
    return unzigzag(v + 1u);
}

/// The activity band: 0 if the neighbours move less than d, and one more each
/// time that doubles. How rough the ground is round a node says how wrong its
/// prediction will be.
inline int band(std::int64_t activity, int d) {
    const std::int64_t base = d > 1 ? d : 1;
    int k = 0;
    while (k < kBands - 1 && activity >= (base << k)) ++k;
    return k;
}

}  // namespace pyre
