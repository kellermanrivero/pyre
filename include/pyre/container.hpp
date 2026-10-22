#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  container.hpp — the .pyre file: header, grids, tiles, index
// ═════════════════════════════════════════════════════════════════════════════
//
// The layout (docs/FORMAT.md has it byte by byte):
//
//   [header 128 B][grids, 128 B each][strings][pad to 8]   the header
//   [tile][tile]…                                           each: head, then its blocks
//   [entries, 48 B each][block ends, 4 B × blocks a tile]   the index
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
/// Entry flags.
inline constexpr std::uint32_t kTileHoles = 1u;

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

struct FileInfo {
    Params params;
    Method method;
    std::int32_t d = 0;      ///< the file's error limit, in quanta (a tile may have its own)
    double unit = 0.01;      ///< the quantum q, in vertical units (metres)
    double noData = std::numeric_limits<double>::quiet_NaN();   ///< the source's no-data value, for output
    std::vector<GridInfo> grids;
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
        info_ = info;
        sink_ = &sink;
        pos_ = detail::headerBytes(info);
        entries_.clear();
        ends_.clear();
        const std::vector<std::uint8_t> zeros(pos_, 0);
        return sink.append(zeros.data(), zeros.size()) ? Status::Ok : Status::Io;
    }

    /// Adds tile (ti, tj) of grid `grid`, encoded with error limit d.
    Status add(std::uint32_t grid, std::int32_t ti, std::int32_t tj, const EncodedTile& t, std::int32_t d) {
        if (!sink_) return Status::Usage;
        if (grid >= info_.grids.size() || t.blocks.size() != info_.params.blocks()) return Status::Usage;
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
            ends_.push_back(end);
        }
        e.crc = crc;
        pos_ += total;
        entries_.push_back(e);
        return Status::Ok;
    }

    /// Writes the index and the header. Two tiles at one place are an error.
    Status finish() {
        if (!sink_) return Status::Usage;
        const std::size_t n = entries_.size(), nb = info_.params.blocks();
        std::vector<std::size_t> order(n);
        for (std::size_t k = 0; k < n; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const TileEntry& y = entries_[b];
            return detail::entryLess(entries_[a], y.grid, y.ti, y.tj);
        });
        for (std::size_t k = 1; k < n; ++k) {
            const TileEntry &a = entries_[order[k - 1]], &b = entries_[order[k]];
            if (a.grid == b.grid && a.ti == b.ti && a.tj == b.tj) return Status::BadInput;
        }
        // The index starts on a multiple of 8.
        const std::size_t padding = detail::pad8(pos_) - pos_;
        const std::uint8_t zeros[8] = {};
        if (!sink_->append(zeros, padding)) return Status::Io;
        pos_ += padding;
        std::vector<std::uint8_t> index(n * kEntryBytes + n * nb * 4);
        for (std::size_t k = 0; k < n; ++k) {
            detail::putEntry(index.data() + k * kEntryBytes, entries_[order[k]]);
            std::memcpy(index.data() + n * kEntryBytes + k * nb * 4, ends_.data() + order[k] * nb, nb * 4);
        }
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
        detail::set(p + 96, pos_);
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

    std::size_t tiles() const { return entries_.size(); }

private:
    FileInfo info_;
    Sink* sink_ = nullptr;
    std::uint64_t pos_ = 0;
    std::vector<TileEntry> entries_;
    std::vector<std::uint32_t> ends_;   ///< blocks() a tile, in the order added
};

/// Reads a file in memory or mapped. open() checks the header, the index and
/// every bound once; after it, every accessor is safe. The reader does not
/// own the bytes. It is const after open: threads may share it.
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
    std::size_t tiles() const { return count_; }
    std::size_t headerBytes() const { return headerBytes_; }
    std::uint64_t indexBytes() const { return indexBytes_; }

    TileEntry entry(std::size_t k) const { return detail::getEntry(file_.data() + indexOffset_ + k * kEntryBytes); }

    /// The index of tile (ti, tj) of a grid, if the file has it.
    std::optional<std::size_t> find(std::uint32_t grid, std::int32_t ti, std::int32_t tj) const {
        std::size_t lo = 0, hi = count_;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (detail::entryLess(entry(mid), grid, ti, tj)) lo = mid + 1;
            else hi = mid;
        }
        if (lo < count_) {
            const TileEntry e = entry(lo);
            if (e.grid == grid && e.ti == ti && e.tj == tj) return lo;
        }
        return std::nullopt;
    }

    TileSpec spec(std::size_t k) const {
        const TileEntry e = entry(k);
        return TileSpec{info_.params, info_.method, e.d, (e.flags & kTileHoles) != 0};
    }

    /// The tile's bytes: its head, then its blocks.
    std::span<const std::uint8_t> tileBytes(std::size_t k) const {
        const TileEntry e = entry(k);
        return file_.subspan(std::size_t(e.offset), e.totalBytes);
    }
    std::span<const std::uint8_t> head(std::size_t k) const { return tileBytes(k).first(entry(k).headBytes); }
    /// Block b (rows south to north, each west to east): its byte range in the
    /// file comes from the index alone.
    std::span<const std::uint8_t> block(std::size_t k, std::size_t b) const {
        const std::uint32_t begin = b == 0 ? entry(k).headBytes : blockEnd(k, b - 1), end = blockEnd(k, b);
        return tileBytes(k).subspan(begin, end - begin);
    }

    /// True if the tile's bytes match their checksum.
    bool verifyTile(std::size_t k) const {
        const auto t = tileBytes(k);
        return crc32c(t.data(), t.size()) == entry(k).crc;
    }

    Status decodeHead(std::size_t k, std::span<std::int32_t> out, std::span<std::uint8_t> mask, std::uint32_t minStep,
                      Scratch& s) const {
        if (k >= count_) return Status::Usage;
        return pyre::decodeHead(spec(k), head(k), out, mask, minStep, s);
    }

    Status decodeBlock(std::size_t k, std::uint32_t bx, std::uint32_t by, std::span<std::int32_t> out,
                       std::span<std::uint8_t> mask, std::uint32_t minStep, Scratch& s) const {
        if (k >= count_) return Status::Usage;
        const std::uint32_t kb = info_.params.blocksPerSide();
        if (bx >= kb || by >= kb) return Status::Usage;
        return pyre::decodeBlock(spec(k), block(k, std::size_t(by) * kb + bx), out, mask, bx, by, minStep, s);
    }

    /// Decodes what the window [i0, i1] × [j0, j1] of tile k needs, down to
    /// minStep: the head, and only the blocks that the window touches. Nodes
    /// outside the window may be decoded too.
    Status decodeWindow(std::size_t k, std::uint32_t i0, std::uint32_t j0, std::uint32_t i1, std::uint32_t j1,
                        std::uint32_t minStep, std::span<std::int32_t> out, std::span<std::uint8_t> mask, Scratch& s) const {
        if (i0 > i1 || j0 > j1 || i1 > info_.params.tile || j1 > info_.params.tile) return Status::Usage;
        Status st = decodeHead(k, out, mask, minStep, s);
        if (st != Status::Ok || minStep >= info_.params.headStep()) return st;
        const BlockRange r = blocksOfWindow(info_.params, i0, j0, i1, j1);
        if (r.empty) return Status::Ok;
        for (std::uint32_t by = r.by0; by <= r.by1; ++by)
            for (std::uint32_t bx = r.bx0; bx <= r.bx1; ++bx)
                if ((st = decodeBlock(k, bx, by, out, mask, minStep, s)) != Status::Ok) return st;
        return Status::Ok;
    }

private:
    std::uint32_t blockEnd(std::size_t k, std::size_t b) const {
        return detail::get<std::uint32_t>(file_.data() + endsOffset_ + (k * info_.params.blocks() + b) * 4);
    }

    Status fail(Status s, std::string why) {
        error_ = std::move(why);
        return s;
    }

    Status parse(std::span<const std::uint8_t> f) {
        error_.clear();
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
        count_ = std::size_t(count);
        endsOffset_ = std::size_t(indexOffset_ + count * kEntryBytes);
        TileEntry prev;
        for (std::size_t k = 0; k < count_; ++k) {
            const TileEntry e = detail::getEntry(p + indexOffset_ + k * kEntryBytes);
            if (e.grid >= gridCount) return fail(Status::Corrupt, "a tile's grid does not exist");
            if (k > 0 && !detail::entryLess(prev, e.grid, e.ti, e.tj)) return fail(Status::Corrupt, "the index is not in order");
            if (e.offset < headerBytes_ || e.offset > indexOffset_ || e.totalBytes > indexOffset_ - e.offset ||
                e.headBytes > e.totalBytes)
                return fail(Status::Corrupt, "a tile is outside the file");
            if (e.d < 0 || e.d > kMaxError || (e.flags & ~kTileHoles) != 0) return fail(Status::Corrupt, "a tile's entry is wrong");
            std::uint32_t at = e.headBytes;
            for (std::size_t b = 0; b < nb; ++b) {
                const std::uint32_t end = detail::get<std::uint32_t>(p + endsOffset_ + (k * nb + b) * 4);
                if (end < at || end > e.totalBytes) return fail(Status::Corrupt, "a block is outside its tile");
                at = end;
            }
            if (at != e.totalBytes) return fail(Status::Corrupt, "a tile's blocks do not fill it");
            prev = e;
        }
        return Status::Ok;
    }

    std::span<const std::uint8_t> file_;
    FileInfo info_;
    std::string error_;
    std::size_t headerBytes_ = 0, count_ = 0, endsOffset_ = 0;
    std::uint64_t indexOffset_ = 0, indexBytes_ = 0;
};

}  // namespace pyre
