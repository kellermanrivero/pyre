// The GeoTIFF writer (see tiff.hpp): float32, one band, 256 × 256 tiles,
// Deflate with predictor 3, little-endian. Classic TIFF when the file surely
// fits under 4 GB, else BigTIFF.

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "tiff.hpp"
#include "tiff_codec.hpp"

namespace pyre::tool {

namespace {

constexpr std::uint32_t kTile = 256;

void put16(std::vector<std::uint8_t>& b, std::uint64_t v) {
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
void putDouble(std::vector<std::uint8_t>& b, double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, 8);
    put64(b, u);
}

struct Tag {
    std::uint16_t tag, type;
    std::uint64_t count;
    std::vector<std::uint8_t> data;   // little-endian
};

Tag shortTag(std::uint16_t tag, const std::vector<std::uint64_t>& v) {
    Tag t{tag, 3, v.size(), {}};
    for (auto x : v) put16(t.data, x);
    return t;
}
Tag longTag(std::uint16_t tag, std::uint64_t v) {
    Tag t{tag, 4, 1, {}};
    put32(t.data, v);
    return t;
}
Tag offsetsTag(std::uint16_t tag, const std::vector<std::uint64_t>& v, bool big) {
    Tag t{tag, std::uint16_t(big ? 16 : 4), v.size(), {}};
    for (auto x : v) big ? put64(t.data, x) : put32(t.data, x);
    return t;
}
Tag doubleTag(std::uint16_t tag, const std::vector<double>& v) {
    Tag t{tag, 12, v.size(), {}};
    for (auto x : v) putDouble(t.data, x);
    return t;
}
Tag asciiTag(std::uint16_t tag, const std::string& s) {
    Tag t{tag, 2, s.size() + 1, {}};
    t.data.assign(s.begin(), s.end());
    t.data.push_back(0);
    return t;
}

std::string formatDouble(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    // Use the shortest text that gives the same double.
    for (int p = 1; p <= 17; ++p) {
        char t[64];
        std::snprintf(t, sizeof t, "%.*g", p, v);
        if (std::strtod(t, nullptr) == v) return t;
    }
    return buf;
}

std::string xmlEscape(const std::string& s) {
    std::string r;
    for (char c : s) switch (c) {
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '&': r += "&amp;"; break;
            case '"': r += "&quot;"; break;
            default: r += c;
        }
    return r;
}

class Out {
public:
    explicit Out(const std::string& path) : path_(path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) throw SourceError("cannot create " + path);
    }
    ~Out() {
        if (f_) {
            std::fclose(f_);
            std::remove(path_.c_str());   // not finished: remove the partial file
        }
    }
    Out(const Out&) = delete;
    Out& operator=(const Out&) = delete;

    std::uint64_t pos() const { return pos_; }
    void write(const void* p, std::size_t n) {
        if (n && std::fwrite(p, 1, n, f_) != n) throw SourceError("write error: " + path_);
        pos_ += n;
    }
    void pad(std::uint64_t align) {
        static const std::uint8_t zeros[8] = {};
        while (pos_ % align) write(zeros, 1);
    }
    void writeAt(std::uint64_t off, const void* p, std::size_t n) {
        if (std::fseek(f_, long(off), SEEK_SET) != 0 || std::fwrite(p, 1, n, f_) != n)
            throw SourceError("write error: " + path_);
        if (std::fseek(f_, 0, SEEK_END) != 0) throw SourceError("seek error: " + path_);
    }
    void close() {
        const bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        if (!ok) {
            std::remove(path_.c_str());
            throw SourceError("write error: " + path_);
        }
    }

private:
    std::string path_;
    std::FILE* f_ = nullptr;
    std::uint64_t pos_ = 0;
};

// Builds one tile (the edges repeat the last column and row), applies
// predictor 3 and compresses it.
void encodeTile(const float* data, std::uint32_t width, std::uint32_t height, std::uint32_t tx, std::uint32_t ty,
                std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& packed) {
    raw.resize(std::size_t(kTile) * kTile * 4);
    for (std::uint32_t r = 0; r < kTile; ++r) {
        const std::uint32_t y = std::min(ty * kTile + r, height - 1);
        const float* src = data + std::size_t(y) * width;
        std::uint8_t* dst = raw.data() + std::size_t(r) * kTile * 4;
        const std::uint32_t x0 = tx * kTile;
        const std::uint32_t n = std::min(kTile, width - x0);
        std::memcpy(dst, src + x0, std::size_t(n) * 4);
        for (std::uint32_t c = n; c < kTile; ++c) std::memcpy(dst + std::size_t(c) * 4, src + width - 1, 4);
    }
    tiffcodec::applyFloatingPoint(raw.data(), kTile, kTile, 4);
    uLongf len = compressBound(uLong(raw.size()));
    packed.resize(len);
    if (compress2(packed.data(), &len, raw.data(), uLong(raw.size()), 6) != Z_OK)
        throw SourceError("deflate: compression failed");
    packed.resize(len);
}

}  // namespace

void writeGeoTiff(const std::string& path, std::uint32_t width, std::uint32_t height, const float* data,
                  const Georef& g, std::optional<double> noData) {
    if (width == 0 || height == 0 || !data) throw SourceError("writeGeoTiff: empty raster");
    const std::uint32_t across = (width + kTile - 1) / kTile, down = (height + kTile - 1) / kTile;
    const std::uint64_t nTiles = std::uint64_t(across) * down;
    // Classic TIFF when even incompressible tiles fit under 4 GB.
    const std::uint64_t bound = nTiles * (compressBound(kTile * kTile * 4) + 16) + (std::uint64_t(1) << 20);
    const bool big = bound >= 0xFFFFFFFFull;

    Out out(path);
    std::vector<std::uint8_t> hdr;
    hdr.push_back('I');
    hdr.push_back('I');
    if (big) {
        put16(hdr, 43);
        put16(hdr, 8);
        put16(hdr, 0);
        put64(hdr, 0);
    } else {
        put16(hdr, 42);
        put32(hdr, 0);
    }
    out.write(hdr.data(), hdr.size());

    // The tiles, compressed in parallel in batches, written in order.
    std::vector<std::uint64_t> offsets(static_cast<std::size_t>(nTiles)), counts(static_cast<std::size_t>(nTiles));
    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    const std::uint64_t batch = std::uint64_t(threads) * 4;
    std::vector<std::vector<std::uint8_t>> packed(static_cast<std::size_t>(batch));
    for (std::uint64_t first = 0; first < nTiles; first += batch) {
        const std::uint64_t n = std::min(batch, nTiles - first);
        std::vector<std::thread> pool;
        std::vector<std::string> errors(threads);
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                try {
                    std::vector<std::uint8_t> raw;
                    for (std::uint64_t k = t; k < n; k += threads) {
                        const std::uint64_t i = first + k;
                        encodeTile(data, width, height, std::uint32_t(i % across), std::uint32_t(i / across), raw,
                                   packed[std::size_t(k)]);
                    }
                } catch (const std::exception& e) {
                    errors[t] = e.what();
                }
            });
        for (auto& th : pool) th.join();
        for (auto& e : errors)
            if (!e.empty()) throw SourceError(e);
        for (std::uint64_t k = 0; k < n; ++k) {
            out.pad(2);
            offsets[std::size_t(first + k)] = out.pos();
            counts[std::size_t(first + k)] = packed[std::size_t(k)].size();
            out.write(packed[std::size_t(k)].data(), packed[std::size_t(k)].size());
        }
    }

    // The GeoKeys, sorted by key.
    std::vector<std::uint64_t> keys = {1, 1, 0, 0};
    std::string ascii;
    auto key = [&](std::uint64_t id, std::uint64_t loc, std::uint64_t count, std::uint64_t value) {
        keys.insert(keys.end(), {id, loc, count, value});
        ++keys[3];
    };
    if (g.model != ModelType::Unknown) key(1024, 0, 1, std::uint64_t(g.model));
    key(1025, 0, 1, std::uint64_t(g.rasterType));
    if (!g.epsg && !g.wkt.empty()) {
        std::string s = g.wkt;
        std::replace(s.begin(), s.end(), '|', '/');
        key(1026, 34737, s.size() + 1, ascii.size());
        ascii += s + "|";
    }
    if (g.epsg && g.model != ModelType::Projected) key(2048, 0, 1, *g.epsg);
    if (g.epsg && g.model == ModelType::Projected) key(3072, 0, 1, *g.epsg);
    if (g.verticalEpsg) key(4096, 0, 1, *g.verticalEpsg);

    std::vector<Tag> tags;
    tags.push_back(longTag(256, width));
    tags.push_back(longTag(257, height));
    tags.push_back(shortTag(258, {32}));
    tags.push_back(shortTag(259, {8}));
    tags.push_back(shortTag(262, {1}));
    tags.push_back(shortTag(277, {1}));
    tags.push_back(shortTag(284, {1}));
    tags.push_back(shortTag(317, {3}));
    tags.push_back(shortTag(322, {kTile}));
    tags.push_back(shortTag(323, {kTile}));
    tags.push_back(offsetsTag(324, offsets, big));
    tags.push_back(offsetsTag(325, counts, big));
    tags.push_back(shortTag(339, {3}));
    tags.push_back(doubleTag(33550, {g.spacingX, g.spacingY, 0.0}));
    tags.push_back(doubleTag(33922, {0.0, 0.0, 0.0, g.originX, g.originY, 0.0}));
    tags.push_back(shortTag(34735, keys));
    if (!ascii.empty()) tags.push_back(asciiTag(34737, ascii));
    if (!g.verticalUnit.empty())
        tags.push_back(asciiTag(42112, "<GDALMetadata>\n  <Item name=\"UNITTYPE\" sample=\"0\" role=\"unittype\">" +
                                           xmlEscape(g.verticalUnit) + "</Item>\n</GDALMetadata>\n"));
    if (noData) tags.push_back(asciiTag(42113, formatDouble(*noData)));

    // The IFD, then the values that do not fit in the entries.
    out.pad(8);
    const std::uint64_t ifdOffset = out.pos();
    const unsigned countSize = big ? 8 : 2, entrySize = big ? 20 : 12, nextSize = big ? 8 : 4, field = big ? 8 : 4;
    std::uint64_t dataPos = ifdOffset + countSize + tags.size() * entrySize + nextSize;
    std::vector<std::uint8_t> ifd, extra;
    big ? put64(ifd, tags.size()) : put16(ifd, tags.size());
    for (const Tag& t : tags) {
        put16(ifd, t.tag);
        put16(ifd, t.type);
        big ? put64(ifd, t.count) : put32(ifd, t.count);
        if (t.data.size() <= field) {
            std::vector<std::uint8_t> f(t.data);
            f.resize(field, 0);
            ifd.insert(ifd.end(), f.begin(), f.end());
        } else {
            while ((dataPos + extra.size()) % 8) extra.push_back(0);
            const std::uint64_t at = dataPos + extra.size();
            big ? put64(ifd, at) : put32(ifd, at);
            extra.insert(extra.end(), t.data.begin(), t.data.end());
        }
    }
    big ? put64(ifd, 0) : put32(ifd, 0);
    if (!big && dataPos + extra.size() > 0xFFFFFFFFull) throw SourceError("writeGeoTiff: file too large for TIFF");
    out.write(ifd.data(), ifd.size());
    out.write(extra.data(), extra.size());

    std::vector<std::uint8_t> ptr;
    big ? put64(ptr, ifdOffset) : put32(ptr, ifdOffset);
    out.writeAt(big ? 8 : 4, ptr.data(), ptr.size());
    out.close();
}

}  // namespace pyre::tool
