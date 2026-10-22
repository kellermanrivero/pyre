#pragma once

// Synthetic terrain for the tests: deterministic on every platform (integers
// and a fixed generator; the float math here only makes test inputs, and the
// tests never compare them across machines).

#include <cmath>
#include <cstdint>
#include <vector>

namespace synth {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int range(int lo, int hi) { return lo + int(next() % std::uint64_t(hi - lo + 1)); }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
};

enum class Kind { Smooth, Rough, Constant, Mountain, Sea };

/// (side × side) heights in cm, row j major. Node (i, j) of tile (ti, tj) is
/// global node (ti·(side−1) + i, tj·(side−1) + j), so neighbour tiles share
/// their edges exactly.
inline std::vector<std::int32_t> terrain(Kind kind, int side, std::uint64_t seed, int ti = 0, int tj = 0) {
    std::vector<std::int32_t> x(std::size_t(side) * side);
    Rng r(seed);
    const double p1 = r.unit() * 6.28, p2 = r.unit() * 6.28, p3 = r.unit() * 6.28;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) {
            const double gx = ti * (side - 1) + i, gy = tj * (side - 1) + j;
            double h = 0;
            switch (kind) {
                case Kind::Smooth: h = 60000 + 3000 * std::sin(gx / 150 + p1) * std::cos(gy / 230 + p2) + 0.8 * gx; break;
                case Kind::Rough: {
                    // A hash of the global node, so neighbours agree on edges.
                    std::uint64_t z = std::uint64_t(gx) * 0x9E3779B1u ^ std::uint64_t(gy) * 0x85EBCA77u ^ seed;
                    z = (z ^ (z >> 29)) * 0xBF58476D1CE4E5B9ull;
                    h = 50000 + 4000 * std::sin(gx / 37 + p1) + double(z % 600);
                    break;
                }
                case Kind::Constant: h = 12345; break;
                case Kind::Mountain:
                    h = 150000 + 80000 * std::sin(gx / 90 + p1) * std::sin(gy / 70 + p2) + 20000 * std::sin(gx / 13 + gy / 17 + p3);
                    break;
                case Kind::Sea: h = std::max(0.0, 2000 * std::sin(gx / 200 + p1) + gy * 3 - 1500); break;
            }
            x[std::size_t(j) * side + i] = std::int32_t(std::lround(h));
        }
    return x;
}

/// A validity mask with holes: single nodes, and a region "outside the sheet".
inline std::vector<std::uint8_t> holes(int side, std::uint64_t seed, int singles, bool region) {
    std::vector<std::uint8_t> m(std::size_t(side) * side, 1);
    Rng r(seed);
    for (int k = 0; k < singles; ++k) m[std::size_t(r.range(0, side * side - 1))] = 0;
    if (region)
        for (int j = 0; j < side; ++j)
            for (int i = 0; i < side; ++i)
                if (i + 2 * j < side / 2 + int(r.next() % 3)) m[std::size_t(j) * side + i] = 0;
    return m;
}

}  // namespace synth
