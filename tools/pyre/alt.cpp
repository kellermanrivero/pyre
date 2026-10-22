#include "alt.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "files.hpp"

namespace pyre::tool {

namespace {
template <class T>
T at(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
}  // namespace

AltPack::AltPack(const std::string& path) : map_(std::make_unique<MappedFile>(path)) {
    const auto b = map_->bytes();
    const std::uint8_t* p = b.data();
    if (b.size() < kHeaderBytes) throw SourceError(path + ": too short for a heights pack");
    if (at<std::uint32_t>(p) != kMagic) throw SourceError(path + ": not a heights pack (no MALT magic)");
    const std::uint32_t version = at<std::uint32_t>(p + 4);
    if (version != kVersion)
        throw SourceError(path + ": heights pack version " + std::to_string(version) + "; only version 3 is read");
    tileM_ = at<double>(p + 8);
    lat0_ = at<double>(p + 16);
    lon0_ = at<double>(p + 24);
    errorCm_ = at<std::int32_t>(p + 32);
    const std::uint32_t nodes = at<std::uint32_t>(p + 36), count = at<std::uint32_t>(p + 40);
    if (nodes != kNodes) throw SourceError(path + ": a tile must have 1001 nodes a side");
    if (!(tileM_ > 0) || !std::isfinite(lat0_) || !std::isfinite(lon0_) || errorCm_ < 0 || errorCm_ > kMaxError)
        throw SourceError(path + ": the header is not valid");
    if (std::uint64_t(count) * kEntryBytes > b.size() - kHeaderBytes) throw SourceError(path + ": the index is cut short");
    entries_.resize(count);
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t* q = p + kHeaderBytes + k * kEntryBytes;
        AltEntry& e = entries_[k];
        e.i = at<std::int32_t>(q);
        e.j = at<std::int32_t>(q + 4);
        e.offset = at<std::uint64_t>(q + 8);
        e.bytes = at<std::uint32_t>(q + 16);
        e.minCm = at<std::int32_t>(q + 20);
        e.maxCm = at<std::int32_t>(q + 24);
        e.errorCm = at<std::uint32_t>(q + 28);
        if (e.offset > b.size() || e.bytes > b.size() - e.offset || e.errorCm > std::uint32_t(kMaxError))
            throw SourceError(path + ": tile " + std::to_string(k) + " is outside the file");
        if (k > 0 && std::make_pair(entries_[k - 1].j, entries_[k - 1].i) >= std::make_pair(e.j, e.i))
            throw SourceError(path + ": the index is not sorted by (j, i)");
    }
}

AltPack::~AltPack() = default;

std::uint64_t AltPack::fileBytes() const { return map_->bytes().size(); }

std::optional<std::size_t> AltPack::find(std::int32_t i, std::int32_t j) const {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), std::make_pair(j, i),
                                     [](const AltEntry& e, const std::pair<std::int32_t, std::int32_t>& key) {
                                         return std::make_pair(e.j, e.i) < key;
                                     });
    if (it == entries_.end() || it->i != i || it->j != j) return std::nullopt;
    return std::size_t(it - entries_.begin());
}

bool AltPack::decode(std::size_t k, std::vector<std::int32_t>& out, Scratch& s) const {
    const Params p = params();
    const AltEntry& e = entries_[k];
    const std::uint8_t* t = map_->bytes().data() + e.offset;
    const std::size_t n = e.bytes;
    const std::uint32_t nb = std::uint32_t(p.blocks());
    if (n < 4) return false;
    const std::uint32_t headBytes = at<std::uint32_t>(t);
    if (headBytes > n - 4 || std::size_t(nb) * 4 > n - 4 - headBytes) return false;
    out.resize(p.nodes());
    std::vector<std::uint8_t> none;
    const TileSpec spec{p, Method::reference(), errorOf(k), false};
    if (decodeHead(spec, {t + 4, headBytes}, out, none, 1, s) != Status::Ok) return false;
    std::size_t pos = 4 + std::size_t(headBytes) + 4 * std::size_t(nb);
    for (std::uint32_t b = 0; b < nb; ++b) {
        const std::uint32_t len = at<std::uint32_t>(t + 4 + headBytes + 4 * b);
        if (len > n - pos) return false;
        if (decodeBlock(spec, {t + pos, len}, out, none, b % p.blocksPerSide(), b / p.blocksPerSide(), 1, s) != Status::Ok)
            return false;
        pos += len;
    }
    return pos == n;
}

}  // namespace pyre::tool
