#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  alt.hpp — the reference heights packs (format 3, magic "MALT", file .alt)
// ═════════════════════════════════════════════════════════════════════════════
//
// A pack is a header (48 B), an index (32 B a tile, sorted by (j, i)) and the
// tiles. A tile is 1001 × 1001 heights in cm, every tileM / 1000 metres, on the
// local grid round (lat0, lon0) (local.hpp): tile (i, j) has its south-west node
// at (i · tileM, j · tileM) metres. Its bytes are
//
//     [u32 head length][head][25 × u32 block length, rows south to north][blocks]
//
// and its streams are those of pyre's reference method (Method::reference()
// with S = 1000, B = 200, G = 8, one block level). So pyre decodes them with
// its own codec. The pack has no no-data.
//
//   Header: u32 magic 0x544C414D, u32 version 3, f64 tileM, f64 lat0, f64 lon0,
//           i32 errorCm, u32 nodes (1001), u32 tiles, u32 reserved.
//   Entry:  i32 i, i32 j, u64 offset, u32 bytes, i32 minCm, i32 maxCm,
//           u32 errorCm (the tile's own error; 0: the header's).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pyre/tile.hpp"

namespace pyre::tool {

class MappedFile;

struct AltEntry {
    std::int32_t i = 0, j = 0;
    std::uint64_t offset = 0;
    std::uint32_t bytes = 0;
    std::int32_t minCm = 0, maxCm = 0;
    std::uint32_t errorCm = 0;   ///< 0: the header's
};

class AltPack {
public:
    static constexpr std::uint32_t kMagic = 0x544C414Du;   // "MALT"
    static constexpr std::uint32_t kVersion = 3;
    static constexpr std::uint32_t kNodes = 1001;
    static constexpr std::size_t kHeaderBytes = 48, kEntryBytes = 32;

    /// Opens a pack and checks its header and index. Throws SourceError.
    explicit AltPack(const std::string& path);
    ~AltPack();

    double tileM() const { return tileM_; }
    double lat0() const { return lat0_; }
    double lon0() const { return lon0_; }
    std::int32_t errorCm() const { return errorCm_; }
    std::size_t tiles() const { return entries_.size(); }
    std::uint64_t fileBytes() const;
    const AltEntry& entry(std::size_t k) const { return entries_[k]; }
    std::optional<std::size_t> find(std::int32_t i, std::int32_t j) const;
    /// The tile's error limit in cm: its own, or the header's.
    std::int32_t errorOf(std::size_t k) const { return entries_[k].errorCm ? std::int32_t(entries_[k].errorCm) : errorCm_; }

    /// The codec parameters of a pack's tiles.
    static Params params() { return Params{1000, 200, 8, 1}; }

    /// Decodes tile k into `out` (1001² heights in cm, row j northwards).
    /// Returns false if the tile is damaged. Thread-safe with one Scratch a thread.
    bool decode(std::size_t k, std::vector<std::int32_t>& out, Scratch& s) const;

private:
    std::unique_ptr<MappedFile> map_;
    double tileM_ = 0, lat0_ = 0, lon0_ = 0;
    std::int32_t errorCm_ = 0;
    std::vector<AltEntry> entries_;
};

}  // namespace pyre::tool
