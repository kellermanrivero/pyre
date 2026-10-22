#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  walk.hpp — one order of the nodes, the same to encode and to decode
// ═════════════════════════════════════════════════════════════════════════════
//
// The walk visits each node once. It computes the node's prediction and
// contexts from nodes that are done already, and asks the visitor for the
// node's value: the encoder's visitor quantises the true height and writes it,
// the decoder's reads it. One walk for both: they cannot get out of step.
//
// The order of a tile (S cells, G the base step, L = log2 G levels):
//
//   The head
//     1. The four corners, as they are.
//     2. The four sides (south, north, west, east), each from its own nodes only
//        and in a fixed direction (west to east, south to north): two tiles
//        that share a side decode the same values on it.
//     3. The inside of the base grid (step G), south-west to north-east, with
//        JPEG-LS's MED predictor.
//     4. Each level above the block levels: the centres of the squares from
//        their 4 corners, then the edge midpoints from their 4 neighbours in a
//        diamond.
//     5. For each block level: that level's nodes on the block lines, each
//        predicted along its line. So a block depends on the head alone.
//   Each block
//     Its block levels inside its lines, as in 4.
//
// Heights are int32; predictions are integer means with shifts, computed in
// int64 so that no value from a damaged stream can overflow.
//
// Every index that the walk makes is j·(S+1) + i with 0 ≤ i, j ≤ S, by
// construction; the entry points check the sizes of the buffers. So the hot
// loops index raw pointers with no further check.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "model.hpp"
#include "params.hpp"

namespace pyre::detail {

/// The geometry that the walk uses, from valid Params.
struct Geo {
    std::size_t n, u, g, b, kb;
    int levels, firstBlockLevel;

    explicit Geo(const Params& p)
        : n(p.side()), u(p.tile), g(p.base), b(p.block), kb(p.blocksPerSide()),
          levels(p.levels()), firstBlockLevel(p.firstBlockLevel()) {}
};

/// The values and the validity of a tile's nodes. `m` is null for a tile
/// with no holes.
struct Grid {
    std::int32_t* r;
    std::uint8_t* m;
};

inline std::int64_t mean2(std::int64_t a, std::int64_t b) { return (a + b + 1) >> 1; }
inline std::int64_t mean4(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t e) {
    return (a + b + c + e + 2) >> 2;
}
inline std::int64_t med(std::int64_t w, std::int64_t s, std::int64_t sw) {
    const std::int64_t mx = std::max(w, s), mn = std::min(w, s);
    if (sw >= mx) return mn;
    if (sw <= mn) return mx;
    return w + s - sw;
}
inline std::int64_t spread4(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t e) {
    return std::max(std::max(a, b), std::max(c, e)) - std::min(std::min(a, b), std::min(c, e));
}

// A Visitor has:
//   static constexpr bool kHoles;           each node has a validity bit
//   bool corner(std::size_t k, std::int32_t& value);
//   bool node(std::size_t k, std::int64_t pred, int ctx, int mctx, std::int32_t& value);
// and returns whether the node is valid.

template <class V>
inline void put(Grid& g, V& v, std::size_t k, std::int64_t pred, int ctx, int mctx) {
    std::int32_t x;
    const bool ok = v.node(k, pred, ctx, mctx, x);
    g.r[k] = x;
    if constexpr (V::kHoles) g.m[k] = std::uint8_t(ok);
}

/// A line of nodes (a side or a block line): node t is start + t·stride.
struct Line {
    std::size_t start, stride;
    std::size_t at(std::size_t t) const { return start + t * stride; }
};

/// The base level of a side: each node from the one before it.
template <class V>
void lineBase(Grid& g, V& v, const Geo& geo, int d, Line line) {
    const std::size_t s = geo.g;
    for (std::size_t t = s; t < geo.u; t += s) {
        const std::size_t pa = line.at(t - s), pb = t >= 2 * s ? line.at(t - 2 * s) : pa;
        const std::int64_t a = g.r[pa];
        const std::int64_t act = t >= 2 * s ? std::abs(a - std::int64_t(g.r[pb])) : 0;
        int mctx = 0;
        if constexpr (V::kHoles) mctx = maskCtx(0, kMask1D, g.m[pa] + g.m[pb]);
        put(g, v, line.at(t), a, ctx1D(0, band(act, d)), mctx);
    }
}

/// Level l's nodes on a line, each from its two neighbours on the line.
template <class V>
void lineLevel(Grid& g, V& v, const Geo& geo, int d, Line line, int l) {
    const std::size_t h = geo.g >> l;
    for (std::size_t t = h; t < geo.u; t += 2 * h) {
        const std::size_t pa = line.at(t - h), pb = line.at(t + h);
        const std::int64_t a = g.r[pa], b = g.r[pb];
        int mctx = 0;
        if constexpr (V::kHoles) mctx = maskCtx(l, kMask1D, g.m[pa] + g.m[pb]);
        put(g, v, line.at(t), mean2(a, b), ctx1D(l, band(std::abs(a - b), d)), mctx);
    }
}

/// The inside of the base grid, with MED from the west, south and south-west.
template <class V>
void baseGrid(Grid& g, V& v, const Geo& geo, int d) {
    const std::size_t n = geo.n, s = geo.g;
    for (std::size_t j = s; j < geo.u; j += s)
        for (std::size_t i = s; i < geo.u; i += s) {
            const std::size_t k = j * n + i, pw = k - s, ps = k - s * n, psw = ps - s;
            const std::int64_t w = g.r[pw], so = g.r[ps], sw = g.r[psw];
            int mctx = 0;
            if constexpr (V::kHoles) mctx = maskCtx(0, kMaskCentre, g.m[pw] + g.m[ps] + g.m[psw]);
            put(g, v, k, med(w, so, sw), ctx2D(0, 0, band(std::abs(w - sw) + std::abs(so - sw), d)), mctx);
        }
}

/// Level l (step h = G >> l) inside the rectangle [x0, x1] × [y0, y1], whose
/// border is done: the centres, then the edge midpoints.
template <class V>
void refine(Grid& g, V& v, const Geo& geo, int d, int l, std::size_t x0, std::size_t y0, std::size_t x1, std::size_t y1) {
    const std::size_t n = geo.n, h = geo.g >> l, s = 2 * h, hn = h * n;
    const int cC = ctx2D(l, 0, 0), cE = ctx2D(l, 1, 0);
    // The centres, from the four corners of their square.
    for (std::size_t j = y0 + h; j < y1; j += s)
        for (std::size_t i = x0 + h; i < x1; i += s) {
            const std::size_t k = j * n + i;
            const std::int64_t a = g.r[k - hn - h], b = g.r[k - hn + h], c = g.r[k + hn - h], e = g.r[k + hn + h];
            int mctx = 0;
            if constexpr (V::kHoles)
                mctx = maskCtx(l, kMaskCentre, g.m[k - hn - h] + g.m[k - hn + h] + g.m[k + hn - h] + g.m[k + hn + h]);
            put(g, v, k, mean4(a, b, c, e), cC + band(spread4(a, b, c, e), d), mctx);
        }
    // The edge midpoints, from their four neighbours in a diamond.
    for (std::size_t j = y0 + h; j < y1; j += h)
        for (std::size_t i = (((j - y0) / h) % 2 == 1) ? x0 + s : x0 + h; i < x1; i += s) {
            const std::size_t k = j * n + i;
            const std::int64_t a = g.r[k - h], b = g.r[k + h], c = g.r[k - hn], e = g.r[k + hn];
            int mctx = 0;
            if constexpr (V::kHoles) mctx = maskCtx(l, kMaskEdge, g.m[k - h] + g.m[k + h] + g.m[k - hn] + g.m[k + hn]);
            put(g, v, k, mean4(a, b, c, e), cE + band(spread4(a, b, c, e), d), mctx);
        }
}

/// The head of a tile down to the step minStep (a power of 2; 1 for all).
/// The sides are always whole. With a step finer than the head step, the
/// block lines come too, down to minStep.
template <class V>
void walkHead(Grid& g, const Geo& geo, int d, V& v, std::size_t minStep) {
    const std::size_t n = geo.n, u = geo.u;

    // 1. The corners.
    for (std::size_t k : {std::size_t(0), u, u * n, u * n + u}) {
        std::int32_t x;
        const bool ok = v.corner(k, x);
        g.r[k] = x;
        if constexpr (V::kHoles) g.m[k] = std::uint8_t(ok);
    }

    // 2. The sides: south, north (west to east), west, east (south to north).
    for (Line line : {Line{0, 1}, Line{u * n, 1}, Line{0, n}, Line{u, n}}) {
        lineBase(g, v, geo, d, line);
        for (int l = 1; l <= geo.levels; ++l) lineLevel(g, v, geo, d, line, l);
    }

    // 3. The inside of the base grid.
    baseGrid(g, v, geo, d);

    // 4. The levels above the block levels.
    for (int l = 1; l < geo.firstBlockLevel; ++l) {
        if ((geo.g >> l) < minStep) return;
        refine(g, v, geo, d, l, 0, 0, u, u);
    }

    // 5. The block lines, one level at a time.
    for (int l = geo.firstBlockLevel; l <= geo.levels; ++l) {
        if ((geo.g >> l) < minStep) return;
        for (std::size_t b = 1; b < geo.kb; ++b) {
            const std::size_t x = b * geo.b;
            lineLevel(g, v, geo, d, Line{x, n}, l);       // the vertical line, south to north
            lineLevel(g, v, geo, d, Line{x * n, 1}, l);   // the horizontal line, west to east
        }
    }
}

/// Block (bx, by)'s nodes of the block levels, down to minStep. The head must
/// be done down to the same step or finer.
template <class V>
void walkBlock(Grid& g, const Geo& geo, int d, V& v, std::size_t bx, std::size_t by, std::size_t minStep) {
    assert(bx < geo.kb && by < geo.kb);
    const std::size_t x0 = bx * geo.b, y0 = by * geo.b;
    for (int l = geo.firstBlockLevel; l <= geo.levels; ++l) {
        if ((geo.g >> l) < minStep) return;
        refine(g, v, geo, d, l, x0, y0, x0 + geo.b, y0 + geo.b);
    }
}

}  // namespace pyre::detail
