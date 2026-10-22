#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  params.hpp — the shape of the pyramid and of the blocks; the status codes
// ═════════════════════════════════════════════════════════════════════════════
//
// The codec throws no exception (a game may build with exceptions off). Every
// call that can fail returns a Status.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "model.hpp"

namespace pyre {

enum class Status : std::uint8_t {
    Ok = 0,
    BadParams,   ///< the parameters break a rule (Params::valid)
    BadInput,    ///< an encoder's input is not valid (a height out of range, a wrong size)
    Empty,       ///< a tile with no valid node: it is not stored
    Corrupt,     ///< the bytes are damaged (a checksum, a bound, a cut stream)
    Version,     ///< a version or a feature that this reader does not know
    Usage,       ///< a buffer of the wrong size, a block or a level that does not exist
    Io,          ///< a file could not be read or written (the tool only)
};

inline const char* toString(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::BadParams: return "bad parameters";
        case Status::BadInput: return "bad input";
        case Status::Empty: return "the tile has no valid node";
        case Status::Corrupt: return "damaged data";
        case Status::Version: return "unknown version or feature";
        case Status::Usage: return "bad call";
        case Status::Io: return "input/output error";
    }
    return "?";
}

/// The largest tile side, in cells (268 million heights).
inline constexpr std::uint32_t kMaxTile = 16384;
/// The largest absolute height, in quanta: a prediction, its difference and a
/// decoded height then fit with room.
inline constexpr std::int32_t kMaxHeight = 1 << 30;
/// The largest error limit D, in quanta.
inline constexpr std::int32_t kMaxError = 1 << 29;

/// All sides in cells; a tile has tile + 1 nodes on each side.
struct Params {
    std::uint32_t tile = 1000;       ///< S: a multiple of base and of block
    std::uint32_t block = 200;       ///< B: a multiple of 2^blockLevels
    std::uint32_t base = 8;          ///< G: a power of 2, from 2 to 256
    std::uint32_t blockLevels = 1;   ///< 1 to log2(G): the finest levels, cut into blocks

    /// log2(G): the levels are 0 (the base, step G) to this one (step 1).
    constexpr int levels() const { return std::countr_zero(base); }
    constexpr int firstBlockLevel() const { return levels() - int(blockLevels) + 1; }
    /// The finest step that the head holds in the whole tile.
    constexpr std::uint32_t headStep() const { return 1u << blockLevels; }
    constexpr std::size_t side() const { return std::size_t(tile) + 1; }
    constexpr std::size_t nodes() const { return side() * side(); }
    constexpr std::uint32_t blocksPerSide() const { return tile / block; }
    constexpr std::size_t blocks() const { return std::size_t(blocksPerSide()) * blocksPerSide(); }

    /// The rules. B need not be a multiple of G (B = 100 or 500 with G = 8 is
    /// fine): the block lines need only lie on the head's finest grid.
    constexpr bool valid() const {
        if (base < 2 || base > 256 || (base & (base - 1)) != 0) return false;
        if (blockLevels < 1 || int(blockLevels) > levels()) return false;
        if (tile == 0 || tile > kMaxTile || tile % base != 0) return false;
        if (block == 0 || tile % block != 0 || block % headStep() != 0) return false;
        return true;
    }

    friend constexpr bool operator==(const Params&, const Params&) = default;
};

static_assert(Params{}.valid());
static_assert(Params{1000, 100, 8, 1}.valid() && Params{1000, 500, 8, 1}.valid());
static_assert(!Params{1000, 500, 8, 3}.valid() && !Params{1000, 300, 8, 1}.valid());
static_assert(Params{4096, 256, 256, 8}.valid() && !Params{4096, 512, 512, 1}.valid());
static_assert(Params{}.levels() < kMaxLevels);

}  // namespace pyre
