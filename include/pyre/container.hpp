#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  container.hpp — the .pyre file: header, grids, tiles, index
// ═════════════════════════════════════════════════════════════════════════════
//
// The layout (docs/FORMAT.md has it byte by byte):
//
//   [header 128 B][grids, 128 B each][strings][pad to 8]   the header
//   [tile][tile]…                                           each: head, then its blocks
//   [sections][section table]                               only if the file has sections
//   [entries, 48 B each][block ends, 4 B × blocks a tile]   the index
//
// Sections (optional) extend the format: each has a type, and a reader skips
// a type it does not know unless the section is marked "required". Type 1 is a
// layer: another integer grid on the same tiles (layer kind 1: the height of
// objects above the ground, from a surface model). Kind 0 is the main heights:
// the tiles and the index above. A file with no sections has the same bytes
// as before sections existed.
//
// Little-endian, every field on its natural alignment, offsets absolute: a
// reader works on a mapped file as it is, with no pointer to fix. The index is
// at the end so that a writer can stream the tiles; the header says where it
// is. CRC-32C of the header, of the index and of each tile.
//
// Grids. A file holds one or more grids: lattices of nodes, each with its own
// origin and spacing in one CRS. Each tile belongs to one grid. (The MDT02
// sheets do not share a lattice: each sheet is its own grid, and pyre does not
// resample.) Tile (ti, tj) of a grid holds its nodes (I, J) with
// ti·S ≤ I ≤ ti·S + S and tj·S ≤ J ≤ tj·S + S; node (I, J) is at
// (originX + I·spacingX, originY + J·spacingY). J grows northwards.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crc32c.hpp"
#include "params.hpp"
#include "tile.hpp"

namespace pyre {

static_assert(std::endian::native == std::endian::little, "the format is little-endian");

inline constexpr char kMagic[4] = {'P', 'Y', 'R', 'E'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderFixed = 128, kGridBytes = 128, kEntryBytes = 48;
inline constexpr std::size_t kSectionBytes = 32, kLayerHeaderBytes = 64;
/// Entry flags.
inline constexpr std::uint32_t kTileHoles = 1u;
/// Section types, and the flag of a section that a reader must know.
inline constexpr std::uint32_t kSectionLayer = 1;
inline constexpr std::uint32_t kSectionRequired = 1u;
/// Layer kinds (a u8). 0 is the main heights; it is never a layer section.
inline constexpr std::uint8_t kKindHeights = 0;
inline constexpr std::uint8_t kKindAboveGround = 1;   ///< the height of objects above the ground (surface − terrain)
inline constexpr std::uint32_t kMaxLayers = 255;

struct GridInfo {
    std::uint32_t epsg = 0;           ///< the horizontal CRS; 0 if it has no code (see wkt)
    std::uint32_t verticalEpsg = 0;   ///< 0 if not known
    std::uint8_t rasterType = 2;      ///< 1 PixelIsArea, 2 PixelIsPoint (GeoTIFF's codes)
    std::uint8_t modelType = 0;       ///< 0 unknown, 1 projected, 2 geographic, 3 geocentric
    std::uint32_t width = 0, height = 0;          ///< the grid's nodes with data, from node (0, 0)
    std::int32_t tileI0 = 0, tileJ0 = 0;          ///< the first tile
    std::uint32_t tilesX = 0, tilesY = 0;         ///< tiles across and up
    double originX = 0, originY = 0;              ///< node (0, 0): the south-west node
    double spacingX = 1, spacingY = 1;            ///< J grows northwards
    std::string wkt, verticalUnit, name;          ///< name: where the grid came from (a sheet)
};

/// A layer: another integer grid on the file's tiles. Node (i, j) of a layer
/// tile is node (stride·i, stride·j) of the tile; a layer tile has S/stride
/// cells a side, blocks of B/stride and a base step of G/stride. Its values
/// are in quanta of `unit`, within `d` quanta of what was given. The codec
/// does not know what the values mean; `kind` says it.
struct LayerInfo {
    std::uint8_t kind = kKindAboveGround;
    std::uint32_t stride = 1;
    double unit = 1;         ///< the value of one quantum, in vertical units
    std::int32_t d = 0;      ///< the error limit, in quanta
    Method method;
    double param = 0;        ///< for the kind; kind 1: heights under it were set to 0
};

/// The codec parameters of a layer's tiles.
constexpr Params layerParams(const Params& p, std::uint32_t stride) {
    return stride == 0 ? Params{0, 0, 0, 0} : Params{p.tile / stride, p.block / stride, p.base / stride, p.blockLevels};
}

/// True if a layer fits the file's parameters.
inline bool layerValid(const Params& p, const LayerInfo& l) {
    const std::uint32_t r = l.stride;
    return l.kind != kKindHeights && r >= 1 && (r & (r - 1)) == 0 && p.tile % r == 0 && p.block % r == 0 && p.base % r == 0 &&
           layerParams(p, r).valid() && std::isfinite(l.unit) && l.unit > 0 && l.d >= 0 && l.d <= kMaxError && std::isfinite(l.param);
}

struct FileInfo {
    Params params;
    Method method;
    std::int32_t d = 0;      ///< the file's error limit, in quanta (a tile may have its own)
    double unit = 0.01;      ///< the quantum q, in vertical units (metres)
    double noData = std::numeric_limits<double>::quiet_NaN();   ///< the source's no-data value, for output
    std::vector<GridInfo> grids;
    std::vector<LayerInfo> layers;   ///< layer L of the API is layers[L]
};

struct TileEntry {
    std::uint32_t grid = 0;
    std::int32_t ti = 0, tj = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;      ///< the tile's first byte in the file
    std::uint32_t headBytes = 0, totalBytes = 0;
    std::int32_t min = 0, max = 0; ///< decoded valid heights, in quanta
    std::int32_t d = 0;            ///< the tile's error limit, in quanta
    std::uint32_t crc = 0;         ///< CRC-32C of the tile's bytes
};

namespace detail {

template <class T>
inline T get(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T>
inline void set(std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

inline std::size_t pad8(std::size_t n) { return (n + 7) & ~std::size_t(7); }

inline std::size_t headerBytes(const FileInfo& f) {
    std::size_t n = kHeaderFixed + kGridBytes * f.grids.size();
    for (const auto& g : f.grids) n += g.wkt.size() + g.verticalUnit.size() + g.name.size();
    return pad8(n);
}

inline void putEntry(std::uint8_t* p, const TileEntry& e) {
    set(p + 0, e.grid);
    set(p + 4, e.ti);
    set(p + 8, e.tj);
    set(p + 12, e.flags);
    set(p + 16, e.offset);
    set(p + 24, e.headBytes);
    set(p + 28, e.totalBytes);
    set(p + 32, e.min);
    set(p + 36, e.max);
    set(p + 40, e.d);
    set(p + 44, e.crc);
}

inline TileEntry getEntry(const std::uint8_t* p) {
    TileEntry e;
    e.grid = get<std::uint32_t>(p + 0);
    e.ti = get<std::int32_t>(p + 4);
    e.tj = get<std::int32_t>(p + 8);
    e.flags = get<std::uint32_t>(p + 12);
    e.offset = get<std::uint64_t>(p + 16);
    e.headBytes = get<std::uint32_t>(p + 24);
    e.totalBytes = get<std::uint32_t>(p + 28);
    e.min = get<std::int32_t>(p + 32);
    e.max = get<std::int32_t>(p + 36);
    e.d = get<std::int32_t>(p + 40);
    e.crc = get<std::uint32_t>(p + 44);
    return e;
}

/// The order of the index: by grid, then rows south to north, then west to east.
inline bool entryLess(const TileEntry& a, std::uint32_t grid, std::int32_t ti, std::int32_t tj) {
    if (a.grid != grid) return a.grid < grid;
    if (a.tj != tj) return a.tj < tj;
    return a.ti < ti;
}

}  // namespace detail

/// Where a writer puts the bytes: a file, a vector.
class Sink {
public:
    virtual ~Sink() = default;
    /// Appends bytes at the end.
    virtual bool append(const void* data, std::size_t n) = 0;
    /// Writes over bytes already appended.
    virtual bool writeAt(std::uint64_t pos, const void* data, std::size_t n) = 0;
};

class VectorSink : public Sink {
public:
    std::vector<std::uint8_t> bytes;
    bool append(const void* data, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + n);
        return true;
    }
    bool writeAt(std::uint64_t pos, const void* data, std::size_t n) override {
        if (pos + n > bytes.size()) return false;
        std::memcpy(bytes.data() + pos, data, n);
        return true;
    }
};

/// Writes a file: begin, then add the tiles in any order, then finish. Not
/// thread-safe: encode tiles on many threads, add them on one.
class Builder {
public:
    Status begin(const FileInfo& info, Sink& sink) {
        if (!info.params.valid()) return Status::BadParams;
        if (info.d < 0 || info.d > kMaxError || info.grids.empty() || info.grids.size() > 0xFFFFFFu) return Status::BadInput;
        if (info.layers.size() > kMaxLayers) return Status::BadInput;
        for (const auto& l : info.layers)
            if (!layerValid(info.params, l)) return Status::BadParams;
        info_ = info;
        sink_ = &sink;
        pos_ = detail::headerBytes(info);
        main_ = {};
        layers_.assign(info.layers.size(), {});
        const std::vector<std::uint8_t> zeros(pos_, 0);
        return sink.append(zeros.data(), zeros.size()) ? Status::Ok : Status::Io;
    }

    /// Adds tile (ti, tj) of grid `grid`, encoded with error limit d.
    Status add(std::uint32_t grid, std::int32_t ti, std::int32_t tj, const EncodedTile& t, std::int32_t d) {
        return put(main_, info_.params, grid, ti, tj, t, d);
    }

    /// Adds layer L's tile (ti, tj) of grid `grid`, encoded with the layer's
    /// parameters and error limit d. Add it after the tile itself, so that the
    /// two are next to each other in the file.
    Status addLayer(std::size_t layer, std::uint32_t grid, std::int32_t ti, std::int32_t tj, const EncodedTile& t, std::int32_t d) {
        if (layer >= layers_.size()) return Status::Usage;
        return put(layers_[layer], layerParams(info_.params, info_.layers[layer].stride), grid, ti, tj, t, d);
    }

private:
    struct Index {
        std::vector<TileEntry> entries;
        std::vector<std::uint32_t> ends;   ///< blocks() a tile, in the order added
    };

    Status put(Index& x, const Params& p, std::uint32_t grid, std::int32_t ti, std::int32_t tj, const EncodedTile& t, std::int32_t d) {
        if (!sink_) return Status::Usage;
        if (grid >= info_.grids.size() || t.blocks.size() != p.blocks()) return Status::Usage;
        if (d < 0 || d > kMaxError) return Status::BadInput;
        const std::uint64_t total = t.bytes();
        if (total > 0xFFFFFFFFu) return Status::BadInput;
        TileEntry e;
        e.grid = grid;
        e.ti = ti;
        e.tj = tj;
        e.flags = t.holes ? kTileHoles : 0;
        e.offset = pos_;
        e.headBytes = std::uint32_t(t.head.size());
        e.totalBytes = std::uint32_t(total);
        e.min = t.min;
        e.max = t.max;
        e.d = d;
        std::uint32_t crc = crc32c(t.head.data(), t.head.size());
        if (!sink_->append(t.head.data(), t.head.size())) return Status::Io;
        std::uint32_t end = e.headBytes;
        for (const auto& b : t.blocks) {
            crc = crc32c(b.data(), b.size(), crc);
            if (!sink_->append(b.data(), b.size())) return Status::Io;
            end += std::uint32_t(b.size());
            x.ends.push_back(end);
        }
        e.crc = crc;
        pos_ += total;
        x.entries.push_back(e);
        return Status::Ok;
    }

    /// The entries in index order, then their block ends. Empty if two tiles
    /// share a place.
    static bool serialize(const Index& x, std::size_t nb, std::vector<std::uint8_t>& out) {
        const std::size_t n = x.entries.size();
        std::vector<std::size_t> order(n);
        for (std::size_t k = 0; k < n; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const TileEntry& y = x.entries[b];
            return detail::entryLess(x.entries[a], y.grid, y.ti, y.tj);
        });
        for (std::size_t k = 1; k < n; ++k) {
            const TileEntry &a = x.entries[order[k - 1]], &b = x.entries[order[k]];
            if (a.grid == b.grid && a.ti == b.ti && a.tj == b.tj) return false;
        }
        const std::size_t at = out.size();
        out.resize(at + n * kEntryBytes + n * nb * 4);
        for (std::size_t k = 0; k < n; ++k) {
            detail::putEntry(out.data() + at + k * kEntryBytes, x.entries[order[k]]);
            if (nb) std::memcpy(out.data() + at + n * kEntryBytes + k * nb * 4, x.ends.data() + order[k] * nb, nb * 4);
        }
        return true;
    }

    /// Pads the file to a multiple of 8.
    bool pad() {
        const std::size_t padding = detail::pad8(pos_) - pos_;
        const std::uint8_t zeros[8] = {};
        if (!sink_->append(zeros, padding)) return false;
        pos_ += padding;
        return true;
    }

public:
    /// Writes the sections, the index and the header. Two tiles at one place
    /// (in the main tiles or in one layer) are an error.
    Status finish() {
        if (!sink_) return Status::Usage;
        const std::size_t n = main_.entries.size(), nb = info_.params.blocks();

        // The layers: one section each, after the tiles.
        std::vector<std::uint8_t> table;
        for (std::size_t L = 0; L < layers_.size(); ++L) {
            const LayerInfo& l = info_.layers[L];
            std::vector<std::uint8_t> sec(kLayerHeaderBytes, 0);
            std::uint8_t* q = sec.data();
            q[0] = l.kind;
            detail::set(q + 4, l.stride);
            detail::set(q + 8, l.unit);
            detail::set(q + 16, l.d);
            detail::set(q + 20, l.method.bits());
            detail::set(q + 24, l.param);
            detail::set(q + 32, std::uint64_t(layers_[L].entries.size()));
            detail::set(q + 40, std::uint32_t(kEntryBytes));
            if (!serialize(layers_[L], layerParams(info_.params, l.stride).blocks(), sec)) return Status::BadInput;
            if (!pad()) return Status::Io;
            std::uint8_t row[kSectionBytes] = {};
            detail::set(row + 0, kSectionLayer);
            detail::set(row + 4, std::uint32_t(0));
            detail::set(row + 8, pos_);
            detail::set(row + 16, std::uint64_t(sec.size()));
            detail::set(row + 24, crc32c(sec.data(), sec.size()));
            table.insert(table.end(), row, row + kSectionBytes);
            if (!sink_->append(sec.data(), sec.size())) return Status::Io;
            pos_ += sec.size();
        }
        std::uint64_t tableOffset = 0;
        if (!table.empty()) {
            if (!pad()) return Status::Io;
            tableOffset = pos_;
            if (!sink_->append(table.data(), table.size())) return Status::Io;
            pos_ += table.size();
        }

        // The index, last, on a multiple of 8.
        std::vector<std::uint8_t> index;
        if (!serialize(main_, nb, index)) return Status::BadInput;
        if (!pad()) return Status::Io;
        const std::uint64_t indexOffset = pos_;
        if (!sink_->append(index.data(), index.size())) return Status::Io;
        pos_ += index.size();

        std::vector<std::uint8_t> h(detail::headerBytes(info_), 0);
        std::uint8_t* p = h.data();
        std::memcpy(p, kMagic, 4);
        detail::set(p + 4, kVersion);
        detail::set(p + 8, std::uint32_t(h.size()));
        detail::set(p + 16, info_.params.tile);
        detail::set(p + 20, info_.params.block);
        detail::set(p + 24, info_.params.base);
        detail::set(p + 28, info_.params.blockLevels);
        detail::set(p + 32, info_.method.bits());
        detail::set(p + 36, info_.d);
        detail::set(p + 40, info_.unit);
        detail::set(p + 48, info_.noData);
        detail::set(p + 56, std::uint32_t(info_.grids.size()));
        detail::set(p + 64, std::uint64_t(n));
        detail::set(p + 72, indexOffset);
        detail::set(p + 80, std::uint64_t(index.size()));
        detail::set(p + 88, crc32c(index.data(), index.size()));
        detail::set(p + 92, std::uint32_t(table.size() / kSectionBytes));
        detail::set(p + 96, pos_);
        detail::set(p + 104, tableOffset);
        detail::set(p + 112, table.empty() ? std::uint32_t(0) : crc32c(table.data(), table.size()));
        std::size_t str = kHeaderFixed + kGridBytes * info_.grids.size();
        auto putString = [&](std::uint8_t* field, const std::string& s) {
            detail::set(field, std::uint32_t(s.empty() ? 0 : str));
            detail::set(field + 4, std::uint32_t(s.size()));
            std::memcpy(p + str, s.data(), s.size());
            str += s.size();
        };
        for (std::size_t g = 0; g < info_.grids.size(); ++g) {
            const GridInfo& gi = info_.grids[g];
            std::uint8_t* q = p + kHeaderFixed + g * kGridBytes;
            detail::set(q + 0, gi.epsg);
            detail::set(q + 4, gi.verticalEpsg);
            q[8] = gi.rasterType;
            q[9] = gi.modelType;
            putString(q + 12, gi.wkt);
            putString(q + 20, gi.verticalUnit);
            putString(q + 28, gi.name);
            detail::set(q + 36, gi.width);
            detail::set(q + 40, gi.height);
            detail::set(q + 44, gi.tileI0);
            detail::set(q + 48, gi.tileJ0);
            detail::set(q + 52, gi.tilesX);
            detail::set(q + 56, gi.tilesY);
            detail::set(q + 64, gi.originX);
            detail::set(q + 72, gi.originY);
            detail::set(q + 80, gi.spacingX);
            detail::set(q + 88, gi.spacingY);
        }
        detail::set(p + 12, crc32c(h.data(), h.size()));   // with its own field 0
        const Status st = sink_->writeAt(0, h.data(), h.size()) ? Status::Ok : Status::Io;
        sink_ = nullptr;
        return st;
    }

    std::size_t tiles() const { return main_.entries.size(); }
    std::size_t layerTiles(std::size_t layer) const { return layer < layers_.size() ? layers_[layer].entries.size() : 0; }

private:
    FileInfo info_;
    Sink* sink_ = nullptr;
    std::uint64_t pos_ = 0;
    Index main_;
    std::vector<Index> layers_;
};

/// Reads a file in memory or mapped. open() checks the header, the index, the
/// sections and every bound once; after it, every accessor is safe. The
/// reader does not own the bytes. It is const after open: threads may share it.
///
/// The main heights use the functions with no layer. Layer L (0 … layers() − 1,
/// FileInfo::layers[L]) uses the functions named layer…; its node (i, j) is node
/// (stride·i, stride·j) of the tile.
class Reader {
public:
    Status open(std::span<const std::uint8_t> file) {
        file_ = {};
        const Status st = parse(file);
        if (st == Status::Ok) file_ = file;
        return st;
    }

    /// Why open() failed, in words.
    const std::string& error() const { return error_; }
    const FileInfo& info() const { return info_; }
    std::size_t tiles() const { return idx_[0].count; }
    std::size_t headerBytes() const { return headerBytes_; }
    std::uint64_t indexBytes() const { return indexBytes_; }
    /// The count of sections, and of those, the ones this reader skipped (types it does not know).
    std::size_t sections() const { return sections_; }
    std::size_t skippedSections() const { return skipped_; }

    TileEntry entry(std::size_t k) const { return entryOf(idx_[0], k); }
    std::optional<std::size_t> find(std::uint32_t grid, std::int32_t ti, std::int32_t tj) const { return findIn(idx_[0], grid, ti, tj); }
    TileSpec spec(std::size_t k) const { return specOf(idx_[0], k); }

    /// The tile's bytes: its head, then its blocks.
    std::span<const std::uint8_t> tileBytes(std::size_t k) const { return bytesOf(idx_[0], k); }
    std::span<const std::uint8_t> head(std::size_t k) const { return headOf(idx_[0], k); }
    /// Block b (rows south to north, each west to east): its byte range in the
    /// file comes from the index alone.
    std::span<const std::uint8_t> block(std::size_t k, std::size_t b) const { return blockOf(idx_[0], k, b); }

    /// True if the tile's bytes match their checksum.
    bool verifyTile(std::size_t k) const { return verifyIn(idx_[0], k); }

    Status decodeHead(std::size_t k, std::span<std::int32_t> out, std::span<std::uint8_t> mask, std::uint32_t minStep,
                      Scratch& s) const {
        return headIn(idx_[0], k, out, mask, minStep, s);
    }
    Status decodeBlock(std::size_t k, std::uint32_t bx, std::uint32_t by, std::span<std::int32_t> out,
                       std::span<std::uint8_t> mask, std::uint32_t minStep, Scratch& s) const {
        return blockIn(idx_[0], k, bx, by, out, mask, minStep, s);
    }
    /// Decodes what the window [i0, i1] × [j0, j1] of tile k needs, down to
    /// minStep: the head, and only the blocks that the window touches. Nodes
    /// outside the window may be decoded too.
    Status decodeWindow(std::size_t k, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1,
                        std::uint32_t minStep, std::span<std::int32_t> out, std::span<std::uint8_t> mask, Scratch& s) const {
        return windowIn(idx_[0], k, i0, j0, i1, j1, minStep, out, mask, s);
    }

    // ── Layers ──
    std::size_t layers() const { return info_.layers.size(); }
    /// The codec parameters of layer L's tiles.
    Params layerParams(std::size_t L) const { return idx_[L + 1].params; }
    std::size_t layerTiles(std::size_t L) const { return idx_[L + 1].count; }
    TileEntry layerEntry(std::size_t L, std::size_t k) const { return entryOf(idx_[L + 1], k); }
    std::optional<std::size_t> layerFind(std::size_t L, std::uint32_t grid, std::int32_t ti, std::int32_t tj) const {
        return findIn(idx_[L + 1], grid, ti, tj);
    }
    TileSpec layerSpec(std::size_t L, std::size_t k) const { return specOf(idx_[L + 1], k); }
    std::span<const std::uint8_t> layerTileBytes(std::size_t L, std::size_t k) const { return bytesOf(idx_[L + 1], k); }
    std::span<const std::uint8_t> layerBlock(std::size_t L, std::size_t k, std::size_t b) const { return blockOf(idx_[L + 1], k, b); }
    bool verifyLayerTile(std::size_t L, std::size_t k) const { return verifyIn(idx_[L + 1], k); }
    Status decodeLayerHead(std::size_t L, std::size_t k, std::span<std::int32_t> out, std::span<std::uint8_t> mask,
                           std::uint32_t minStep, Scratch& s) const {
        if (L >= layers()) return Status::Usage;
        return headIn(idx_[L + 1], k, out, mask, minStep, s);
    }
    Status decodeLayerBlock(std::size_t L, std::size_t k, std::uint32_t bx, std::uint32_t by, std::span<std::int32_t> out,
                            std::span<std::uint8_t> mask, std::uint32_t minStep, Scratch& s) const {
        if (L >= layers()) return Status::Usage;
        return blockIn(idx_[L + 1], k, bx, by, out, mask, minStep, s);
    }
    /// As decodeWindow, in the layer's own nodes (0 … S/stride).
    Status decodeLayerWindow(std::size_t L, std::size_t k, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1,
                             std::uint32_t minStep, std::span<std::int32_t> out, std::span<std::uint8_t> mask, Scratch& s) const {
        if (L >= layers()) return Status::Usage;
        return windowIn(idx_[L + 1], k, i0, j0, i1, j1, minStep, out, mask, s);
    }

private:
    /// An index: the main one, or a layer's.
    struct Index {
        Params params;
        Method method;
        std::uint64_t entries = 0;      ///< where the entries start
        std::size_t entryBytes = kEntryBytes;
        std::size_t count = 0;
        std::uint64_t ends = 0;         ///< where the block ends start
    };

    TileEntry entryOf(const Index& x, std::size_t k) const {
        return detail::getEntry(file_.data() + x.entries + k * x.entryBytes);
    }
    std::optional<std::size_t> findIn(const Index& x, std::uint32_t grid, std::int32_t ti, std::int32_t tj) const {
        std::size_t lo = 0, hi = x.count;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (detail::entryLess(entryOf(x, mid), grid, ti, tj)) lo = mid + 1;
            else hi = mid;
        }
        if (lo < x.count) {
            const TileEntry e = entryOf(x, lo);
            if (e.grid == grid && e.ti == ti && e.tj == tj) return lo;
        }
        return std::nullopt;
    }
    TileSpec specOf(const Index& x, std::size_t k) const {
        const TileEntry e = entryOf(x, k);
        return TileSpec{x.params, x.method, e.d, (e.flags & kTileHoles) != 0};
    }
    std::span<const std::uint8_t> bytesOf(const Index& x, std::size_t k) const {
        const TileEntry e = entryOf(x, k);
        return file_.subspan(std::size_t(e.offset), e.totalBytes);
    }
    std::span<const std::uint8_t> headOf(const Index& x, std::size_t k) const { return bytesOf(x, k).first(entryOf(x, k).headBytes); }
    std::uint32_t blockEnd(const Index& x, std::size_t k, std::size_t b) const {
        return detail::get<std::uint32_t>(file_.data() + x.ends + (k * x.params.blocks() + b) * 4);
    }
    std::span<const std::uint8_t> blockOf(const Index& x, std::size_t k, std::size_t b) const {
        const std::uint32_t begin = b == 0 ? entryOf(x, k).headBytes : blockEnd(x, k, b - 1), end = blockEnd(x, k, b);
        return bytesOf(x, k).subspan(begin, end - begin);
    }
    bool verifyIn(const Index& x, std::size_t k) const {
        const auto t = bytesOf(x, k);
        return crc32c(t.data(), t.size()) == entryOf(x, k).crc;
    }
    Status headIn(const Index& x, std::size_t k, std::span<std::int32_t> out, std::span<std::uint8_t> mask, std::uint32_t minStep,
                  Scratch& s) const {
        if (k >= x.count) return Status::Usage;
        return pyre::decodeHead(specOf(x, k), headOf(x, k), out, mask, minStep, s);
    }
    Status blockIn(const Index& x, std::size_t k, std::uint32_t bx, std::uint32_t by, std::span<std::int32_t> out,
                   std::span<std::uint8_t> mask, std::uint32_t minStep, Scratch& s) const {
        if (k >= x.count) return Status::Usage;
        const std::uint32_t kb = x.params.blocksPerSide();
        if (bx >= kb || by >= kb) return Status::Usage;
        return pyre::decodeBlock(specOf(x, k), blockOf(x, k, std::size_t(by) * kb + bx), out, mask, bx, by, minStep, s);
    }
    Status windowIn(const Index& x, std::size_t k, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1,
                    std::uint32_t minStep, std::span<std::int32_t> out, std::span<std::uint8_t> mask, Scratch& s) const {
        if (i0 > i1 || j0 > j1 || i1 > x.params.tile || j1 > x.params.tile) return Status::Usage;
        Status st = headIn(x, k, out, mask, minStep, s);
        if (st != Status::Ok || minStep >= x.params.headStep()) return st;
        const BlockRange r = blocksOfWindow(x.params, i0, j0, i1, j1);
        if (r.empty) return Status::Ok;
        for (std::uint32_t by = r.by0; by <= r.by1; ++by)
            for (std::uint32_t bx = r.bx0; bx <= r.bx1; ++bx)
                if ((st = blockIn(x, k, bx, by, out, mask, minStep, s)) != Status::Ok) return st;
        return Status::Ok;
    }

    Status fail(Status s, std::string why) {
        error_ = std::move(why);
        return s;
    }

    /// Checks the entries and block ends of an index; tiles must lie in [headerBytes, tilesEnd).
    Status checkIndex(const std::uint8_t* p, const Index& x, std::uint64_t tilesEnd, std::uint32_t gridCount, const char* what) {
        const std::size_t nb = x.params.blocks();
        TileEntry prev;
        for (std::size_t k = 0; k < x.count; ++k) {
            const TileEntry e = detail::getEntry(p + x.entries + k * x.entryBytes);
            if (e.grid >= gridCount) return fail(Status::Corrupt, std::string(what) + ": a tile's grid does not exist");
            if (k > 0 && !detail::entryLess(prev, e.grid, e.ti, e.tj)) return fail(Status::Corrupt, std::string(what) + ": the index is not in order");
            if (e.offset < headerBytes_ || e.offset > tilesEnd || e.totalBytes > tilesEnd - e.offset || e.headBytes > e.totalBytes)
                return fail(Status::Corrupt, std::string(what) + ": a tile is outside the file");
            if (e.d < 0 || e.d > kMaxError || (e.flags & ~kTileHoles) != 0)
                return fail(Status::Corrupt, std::string(what) + ": a tile's entry is wrong");
            std::uint32_t at = e.headBytes;
            for (std::size_t b = 0; b < nb; ++b) {
                const std::uint32_t end = detail::get<std::uint32_t>(p + x.ends + (k * nb + b) * 4);
                if (end < at || end > e.totalBytes) return fail(Status::Corrupt, std::string(what) + ": a block is outside its tile");
                at = end;
            }
            if (at != e.totalBytes) return fail(Status::Corrupt, std::string(what) + ": a tile's blocks do not fill it");
            prev = e;
        }
        return Status::Ok;
    }

    Status parse(std::span<const std::uint8_t> f) {
        error_.clear();
        idx_.assign(1, Index{});   // an empty main index until the file is good
        sections_ = skipped_ = 0;
        const std::uint8_t* p = f.data();
        const std::size_t n = f.size();
        if (n < kHeaderFixed || std::memcmp(p, kMagic, 4) != 0) return fail(Status::Corrupt, "not a pyre file (no PYRE magic)");
        const std::uint32_t version = detail::get<std::uint32_t>(p + 4);
        if (version != kVersion)
            return fail(Status::Version, "file format version " + std::to_string(version) + "; this reader knows version " +
                                             std::to_string(kVersion) + " only");
        headerBytes_ = detail::get<std::uint32_t>(p + 8);
        const std::uint32_t gridCount = detail::get<std::uint32_t>(p + 56);
        if (headerBytes_ < kHeaderFixed || headerBytes_ > n || headerBytes_ % 8 != 0 ||
            gridCount == 0 || gridCount > (headerBytes_ - kHeaderFixed) / kGridBytes)
            return fail(Status::Corrupt, "the header's size is wrong");
        {
            std::vector<std::uint8_t> h(p, p + headerBytes_);
            detail::set(h.data() + 12, std::uint32_t(0));
            if (crc32c(h.data(), h.size()) != detail::get<std::uint32_t>(p + 12))
                return fail(Status::Corrupt, "the header's checksum is wrong");
        }
        info_ = FileInfo{};
        info_.params = Params{detail::get<std::uint32_t>(p + 16), detail::get<std::uint32_t>(p + 20),
                              detail::get<std::uint32_t>(p + 24), detail::get<std::uint32_t>(p + 28)};
        if (!info_.params.valid()) return fail(Status::Corrupt, "the parameters break the rules");
        if (!Method::fromBits(detail::get<std::uint32_t>(p + 32), info_.method))
            return fail(Status::Version, "the coding method is not known to this reader");
        info_.d = detail::get<std::int32_t>(p + 36);
        info_.unit = detail::get<double>(p + 40);
        info_.noData = detail::get<double>(p + 48);
        if (info_.d < 0 || info_.d > kMaxError || !(info_.unit > 0)) return fail(Status::Corrupt, "the error limit or the unit is wrong");
        if (detail::get<std::uint32_t>(p + 60) != 0) return fail(Status::Version, "the header has flags that this reader does not know");
        const std::uint64_t count = detail::get<std::uint64_t>(p + 64);
        indexOffset_ = detail::get<std::uint64_t>(p + 72);
        indexBytes_ = detail::get<std::uint64_t>(p + 80);
        if (detail::get<std::uint64_t>(p + 96) != n) return fail(Status::Corrupt, "the file is cut short or too long");
        auto stringAt = [&](const std::uint8_t* field, std::string& out) {
            const std::uint32_t at = detail::get<std::uint32_t>(field), len = detail::get<std::uint32_t>(field + 4);
            if (len == 0) return true;
            if (at < kHeaderFixed + kGridBytes * gridCount || std::uint64_t(at) + len > headerBytes_) return false;
            out.assign(reinterpret_cast<const char*>(p + at), len);
            return true;
        };
        for (std::uint32_t g = 0; g < gridCount; ++g) {
            const std::uint8_t* q = p + kHeaderFixed + std::size_t(g) * kGridBytes;
            GridInfo gi;
            gi.epsg = detail::get<std::uint32_t>(q + 0);
            gi.verticalEpsg = detail::get<std::uint32_t>(q + 4);
            gi.rasterType = q[8];
            gi.modelType = q[9];
            if (!stringAt(q + 12, gi.wkt) || !stringAt(q + 20, gi.verticalUnit) || !stringAt(q + 28, gi.name))
                return fail(Status::Corrupt, "a grid's text is outside the header");
            gi.width = detail::get<std::uint32_t>(q + 36);
            gi.height = detail::get<std::uint32_t>(q + 40);
            gi.tileI0 = detail::get<std::int32_t>(q + 44);
            gi.tileJ0 = detail::get<std::int32_t>(q + 48);
            gi.tilesX = detail::get<std::uint32_t>(q + 52);
            gi.tilesY = detail::get<std::uint32_t>(q + 56);
            gi.originX = detail::get<double>(q + 64);
            gi.originY = detail::get<double>(q + 72);
            gi.spacingX = detail::get<double>(q + 80);
            gi.spacingY = detail::get<double>(q + 88);
            info_.grids.push_back(std::move(gi));
        }

        // The index.
        const std::uint64_t nb = info_.params.blocks();
        if (count > (n / kEntryBytes) || indexOffset_ % 8 != 0 || indexOffset_ < headerBytes_ || indexOffset_ > n ||
            indexBytes_ != count * (kEntryBytes + 4 * nb) || indexBytes_ > n - indexOffset_ || indexOffset_ + indexBytes_ != n)
            return fail(Status::Corrupt, "the index is not where the header says");
        if (crc32c(p + indexOffset_, std::size_t(indexBytes_)) != detail::get<std::uint32_t>(p + 88))
            return fail(Status::Corrupt, "the index's checksum is wrong");
        Index main;
        main.params = info_.params;
        main.method = info_.method;
        main.entries = indexOffset_;
        main.count = std::size_t(count);
        main.ends = indexOffset_ + count * kEntryBytes;

        // The sections: a table of 32-byte rows, each with its own checksum. The
        // tiles lie before the first section (or before the index).
        std::uint64_t tilesEnd = indexOffset_;
        const std::uint32_t sectionCount = detail::get<std::uint32_t>(p + 92);
        const std::uint64_t tableOffset = detail::get<std::uint64_t>(p + 104);
        std::vector<Index> layerIdx;
        if (sectionCount > 0) {
            const std::uint64_t tableBytes = std::uint64_t(sectionCount) * kSectionBytes;
            if (tableOffset % 8 != 0 || tableOffset < headerBytes_ || tableOffset > indexOffset_ || tableBytes > indexOffset_ - tableOffset)
                return fail(Status::Corrupt, "the section table is not where the header says");
            if (crc32c(p + tableOffset, std::size_t(tableBytes)) != detail::get<std::uint32_t>(p + 112))
                return fail(Status::Corrupt, "the section table's checksum is wrong");
            tilesEnd = tableOffset;
            for (std::uint32_t k = 0; k < sectionCount; ++k) {
                const std::uint8_t* row = p + tableOffset + std::size_t(k) * kSectionBytes;
                tilesEnd = std::min(tilesEnd, detail::get<std::uint64_t>(row + 8));
            }
            for (std::uint32_t k = 0; k < sectionCount; ++k) {
                const std::uint8_t* row = p + tableOffset + std::size_t(k) * kSectionBytes;
                const std::uint32_t type = detail::get<std::uint32_t>(row + 0), flags = detail::get<std::uint32_t>(row + 4);
                const std::uint64_t at = detail::get<std::uint64_t>(row + 8), bytes = detail::get<std::uint64_t>(row + 16);
                if (at % 8 != 0 || at < headerBytes_ || at > tableOffset || bytes > tableOffset - at)
                    return fail(Status::Corrupt, "section " + std::to_string(k) + " is outside the file");
                if (crc32c(p + at, std::size_t(bytes)) != detail::get<std::uint32_t>(row + 24))
                    return fail(Status::Corrupt, "section " + std::to_string(k) + "'s checksum is wrong");
                ++sections_;
                if (type != kSectionLayer) {
                    if (flags & kSectionRequired)
                        return fail(Status::Version, "the file needs section type " + std::to_string(type) + ", which this reader does not know");
                    ++skipped_;
                    continue;
                }
                // A layer: its descriptor, then its entries and block ends.
                if (info_.layers.size() >= kMaxLayers || bytes < kLayerHeaderBytes) return fail(Status::Corrupt, "a layer section is too short");
                const std::uint8_t* q = p + at;
                LayerInfo l;
                l.kind = q[0];
                l.stride = detail::get<std::uint32_t>(q + 4);
                l.unit = detail::get<double>(q + 8);
                l.d = detail::get<std::int32_t>(q + 16);
                if (!Method::fromBits(detail::get<std::uint32_t>(q + 20), l.method))
                    return fail(Status::Version, "a layer's coding method is not known to this reader");
                l.param = detail::get<double>(q + 24);
                if (!layerValid(info_.params, l)) return fail(Status::Corrupt, "a layer's parameters break the rules");
                Index x;
                x.params = pyre::layerParams(info_.params, l.stride);
                x.method = l.method;
                const std::uint64_t lc = detail::get<std::uint64_t>(q + 32);
                x.entryBytes = detail::get<std::uint32_t>(q + 40);
                const std::uint64_t lnb = x.params.blocks();
                if (x.entryBytes < kEntryBytes || x.entryBytes % 8 != 0 || x.entryBytes > 4096 || lc > bytes / x.entryBytes ||
                    bytes != kLayerHeaderBytes + lc * (x.entryBytes + 4 * lnb))
                    return fail(Status::Corrupt, "a layer's index has the wrong size");
                x.count = std::size_t(lc);
                x.entries = at + kLayerHeaderBytes;
                x.ends = x.entries + lc * x.entryBytes;
                info_.layers.push_back(l);
                layerIdx.push_back(x);
            }
        }
        if (Status st = checkIndex(p, main, tilesEnd, gridCount, "the index"); st != Status::Ok) return st;
        for (std::size_t L = 0; L < layerIdx.size(); ++L)
            if (Status st = checkIndex(p, layerIdx[L], tilesEnd, gridCount, ("layer " + std::to_string(L)).c_str()); st != Status::Ok)
                return st;
        idx_[0] = main;
        idx_.insert(idx_.end(), layerIdx.begin(), layerIdx.end());
        return Status::Ok;
    }

    std::span<const std::uint8_t> file_;
    FileInfo info_;
    std::string error_;
    std::size_t headerBytes_ = 0, sections_ = 0, skipped_ = 0;
    std::uint64_t indexOffset_ = 0, indexBytes_ = 0;
    std::vector<Index> idx_ = std::vector<Index>(1);   ///< [0] the main heights, [1 + L] layer L
};

}  // namespace pyre
