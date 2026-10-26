// The GeoTIFF / COG reader (see tiff.hpp).

#include "tiff.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "tiff_codec.hpp"

namespace pyre::tool {

namespace {

// ── Limits (a damaged file must not make us allocate without bound) ──────────

constexpr std::uint64_t kMaxIfdEntries = 4096;
constexpr int kMaxIfds = 4096;
constexpr std::uint64_t kMaxTagBytes = std::uint64_t(1) << 30;      // one tag's data
constexpr std::uint64_t kMaxBlockBytes = std::uint64_t(1) << 30;    // one decoded block
constexpr std::uint64_t kMaxBlocks = std::uint64_t(1) << 28;

// TIFF tags.
enum : std::uint16_t {
    kNewSubfileType = 254,
    kSubfileType = 255,
    kImageWidth = 256,
    kImageLength = 257,
    kBitsPerSample = 258,
    kCompression = 259,
    kStripOffsets = 273,
    kSamplesPerPixel = 277,
    kRowsPerStrip = 278,
    kStripByteCounts = 279,
    kPlanarConfig = 284,
    kPredictor = 317,
    kTileWidth = 322,
    kTileLength = 323,
    kTileOffsets = 324,
    kTileByteCounts = 325,
    kSampleFormat = 339,
    kModelPixelScale = 33550,
    kModelTiepoint = 33922,
    kModelTransformation = 34264,
    kGeoKeyDirectory = 34735,
    kGeoDoubleParams = 34736,
    kGeoAsciiParams = 34737,
    kGdalMetadata = 42112,
    kGdalNoData = 42113,
};

unsigned typeSize(std::uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;   // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                   // SHORT SSHORT
        case 4: case 9: case 11: case 13: return 4; // LONG SLONG FLOAT IFD
        case 5: case 10: case 12: case 16: case 17: case 18: return 8;  // RATIONAL SRATIONAL DOUBLE LONG8 SLONG8 IFD8
        default: return 0;
    }
}

// ── The file: pread only, so many threads can read at once ───────────────────

class File {
public:
    explicit File(const std::string& path) : path_(path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) throw SourceError("cannot open " + path + ": " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd_);
            throw SourceError("not a regular file: " + path);
        }
        size_ = std::uint64_t(st.st_size);
    }
    ~File() {
        if (fd_ >= 0) ::close(fd_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::uint64_t size() const { return size_; }
    const std::string& path() const { return path_; }

    bool inside(std::uint64_t off, std::uint64_t n) const { return off <= size_ && n <= size_ - off; }

    void read(std::uint64_t off, void* dst, std::uint64_t n) const {
        if (!inside(off, n)) throw SourceError(path_ + ": data out of the file (truncated or damaged)");
        auto* p = static_cast<std::uint8_t*>(dst);
        while (n > 0) {
            const std::size_t chunk = std::size_t(std::min<std::uint64_t>(n, std::uint64_t(1) << 30));
            const ssize_t r = ::pread(fd_, p, chunk, off_t(off));
            if (r < 0) {
                if (errno == EINTR) continue;
                throw SourceError(path_ + ": read error: " + std::strerror(errno));
            }
            if (r == 0) throw SourceError(path_ + ": unexpected end of file");
            p += r;
            off += std::uint64_t(r);
            n -= std::uint64_t(r);
        }
    }

private:
    std::string path_;
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

// ── Byte order ───────────────────────────────────────────────────────────────

struct Order {
    bool big = false;
    std::uint16_t u16(const std::uint8_t* p) const {
        return big ? std::uint16_t(p[0] << 8 | p[1]) : std::uint16_t(p[1] << 8 | p[0]);
    }
    std::uint32_t u32(const std::uint8_t* p) const {
        return big ? (std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3])
                   : (std::uint32_t(p[3]) << 24 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[1]) << 8 | p[0]);
    }
    std::uint64_t u64(const std::uint8_t* p) const {
        return big ? (std::uint64_t(u32(p)) << 32 | u32(p + 4)) : (std::uint64_t(u32(p + 4)) << 32 | u32(p));
    }
};

// ── IFD entries ──────────────────────────────────────────────────────────────

struct Entry {
    std::uint16_t tag = 0, type = 0;
    std::uint64_t count = 0;
    std::uint8_t field[8] = {};   // the value field as stored (4 or 8 bytes)
    bool isInline = false;
    std::uint64_t offset = 0;     // when not inline
};

class Parser {
public:
    Parser(const File& f, Order o, bool bigTiff) : f_(f), o_(o), big_(bigTiff) {}

    /// Reads one IFD; returns the entries and sets `next`.
    std::map<std::uint16_t, Entry> readIfd(std::uint64_t off, std::uint64_t& next) const {
        const unsigned countSize = big_ ? 8 : 2, entrySize = big_ ? 20 : 12, nextSize = big_ ? 8 : 4;
        std::uint8_t buf[8];
        f_.read(off, buf, countSize);
        const std::uint64_t n = big_ ? o_.u64(buf) : o_.u16(buf);
        if (n == 0 || n > kMaxIfdEntries) throw SourceError(f_.path() + ": bad IFD entry count");
        std::vector<std::uint8_t> raw(std::size_t(n * entrySize + nextSize));
        f_.read(off + countSize, raw.data(), raw.size());
        std::map<std::uint16_t, Entry> entries;
        const unsigned fieldSize = big_ ? 8 : 4;
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::uint8_t* p = raw.data() + i * entrySize;
            Entry e;
            e.tag = o_.u16(p);
            e.type = o_.u16(p + 2);
            e.count = big_ ? o_.u64(p + 4) : o_.u32(p + 4);
            const std::uint8_t* field = p + (big_ ? 12 : 8);
            std::memcpy(e.field, field, fieldSize);
            const unsigned ts = typeSize(e.type);
            if (ts == 0) continue;   // an unknown type: ignore the entry
            if (e.count > kMaxTagBytes / ts) throw SourceError(f_.path() + ": tag too large");
            const std::uint64_t bytes = e.count * ts;
            e.isInline = bytes <= fieldSize;
            if (!e.isInline) {
                e.offset = big_ ? o_.u64(field) : o_.u32(field);
                if (!f_.inside(e.offset, bytes)) throw SourceError(f_.path() + ": tag data out of the file");
            }
            entries.emplace(e.tag, e);   // the first entry of a tag wins
        }
        const std::uint8_t* np = raw.data() + n * entrySize;
        next = big_ ? o_.u64(np) : o_.u32(np);
        return entries;
    }

    std::vector<std::uint8_t> bytes(const Entry& e) const {
        const std::uint64_t n = e.count * typeSize(e.type);
        std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
        if (e.isInline)
            std::memcpy(v.data(), e.field, std::size_t(n));
        else
            f_.read(e.offset, v.data(), n);
        return v;
    }

    /// Integer values (BYTE, SHORT, LONG, LONG8, IFD, and the signed ones).
    std::vector<std::uint64_t> ints(const Entry& e) const {
        const auto b = bytes(e);
        std::vector<std::uint64_t> v(std::size_t(e.count));
        const std::uint8_t* p = b.data();
        for (std::size_t i = 0; i < v.size(); ++i) switch (e.type) {
                case 1: case 7: v[i] = p[i]; break;
                case 6: v[i] = std::uint64_t(std::int64_t(std::int8_t(p[i]))); break;
                case 3: v[i] = o_.u16(p + 2 * i); break;
                case 8: v[i] = std::uint64_t(std::int64_t(std::int16_t(o_.u16(p + 2 * i)))); break;
                case 4: case 13: v[i] = o_.u32(p + 4 * i); break;
                case 9: v[i] = std::uint64_t(std::int64_t(std::int32_t(o_.u32(p + 4 * i)))); break;
                case 16: case 17: case 18: v[i] = o_.u64(p + 8 * i); break;
                default: throw SourceError(f_.path() + ": tag " + std::to_string(e.tag) + " is not an integer");
            }
        return v;
    }

    std::vector<double> doubles(const Entry& e) const {
        if (e.type != 5 && e.type != 10 && e.type != 11 && e.type != 12) {
            // Integers are allowed where doubles are expected.
            const auto iv = ints(e);
            const bool isSigned = e.type == 6 || e.type == 8 || e.type == 9 || e.type == 17;
            std::vector<double> d(iv.size());
            for (std::size_t k = 0; k < iv.size(); ++k) d[k] = isSigned ? double(std::int64_t(iv[k])) : double(iv[k]);
            return d;
        }
        const auto b = bytes(e);
        std::vector<double> v(std::size_t(e.count));
        const std::uint8_t* p = b.data();
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (e.type == 11) {
                const std::uint32_t u = o_.u32(p + 4 * i);
                float x;
                std::memcpy(&x, &u, 4);
                v[i] = x;
            } else if (e.type == 12) {
                const std::uint64_t u = o_.u64(p + 8 * i);
                std::memcpy(&v[i], &u, 8);
            } else if (e.type == 5) {
                const std::uint32_t num = o_.u32(p + 8 * i), den = o_.u32(p + 8 * i + 4);
                v[i] = den ? double(num) / double(den) : 0.0;
            } else {
                const auto num = std::int32_t(o_.u32(p + 8 * i)), den = std::int32_t(o_.u32(p + 8 * i + 4));
                v[i] = den ? double(num) / double(den) : 0.0;
            }
        }
        return v;
    }

    std::string ascii(const Entry& e) const {
        if (e.type != 2 && e.type != 1 && e.type != 7) throw SourceError(f_.path() + ": tag is not ASCII");
        const auto b = bytes(e);
        std::string s(b.begin(), b.end());
        const auto z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        return s;
    }

private:
    const File& f_;
    Order o_;
    bool big_;
};

std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::optional<double> parseNoData(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return std::nullopt;
    return v;
}

std::string unitFromGdalMetadata(const std::string& xml) {
    const auto k = xml.find("name=\"UNITTYPE\"");
    if (k == std::string::npos) return {};
    const auto gt = xml.find('>', k);
    if (gt == std::string::npos) return {};
    const auto close = xml.find("</Item>", gt);
    if (close == std::string::npos) return {};
    return trim(xml.substr(gt + 1, close - gt - 1));
}

// ── Decoded blocks and the cache ─────────────────────────────────────────────

struct Block {
    std::unique_ptr<std::uint8_t[]> data;   // native-endian samples, blockWidth per row
    std::size_t size = 0;
};
using BlockPtr = std::shared_ptr<const Block>;

template <class T>
inline T loadAs(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
void convertRow(const std::uint8_t* src, double* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] = double(loadAs<T>(src + i * sizeof(T)));
}

using ConvertFn = void (*)(const std::uint8_t*, double*, std::size_t);

}  // namespace

// ── TiffSource::Impl ─────────────────────────────────────────────────────────

struct TiffSource::Impl {
    File file;
    SourceInfo info;
    TiffLayout layout;
    unsigned bytesPerSample = 0;
    std::vector<std::uint64_t> offsets, counts;
    ConvertFn convert = nullptr;

    mutable std::mutex cacheMutex;
    mutable std::size_t cacheLimit = kDefaultCacheBlocks;
    mutable std::list<std::pair<std::uint64_t, BlockPtr>> lru;   // the most recent first
    mutable std::unordered_map<std::uint64_t, decltype(lru)::iterator> index;

    explicit Impl(const std::string& path) : file(path) {}

    void parse();
    BlockPtr decodeBlock(std::uint64_t b) const;
    BlockPtr getBlock(std::uint64_t b) const;
};

void TiffSource::Impl::parse() {
    const std::string& path = file.path();
    std::uint8_t hdr[16];
    if (file.size() < 8) throw SourceError(path + ": not a TIFF file (too short)");
    file.read(0, hdr, std::min<std::uint64_t>(16, file.size()));
    Order o;
    if (hdr[0] == 'I' && hdr[1] == 'I')
        o.big = false;
    else if (hdr[0] == 'M' && hdr[1] == 'M')
        o.big = true;
    else
        throw SourceError(path + ": not a TIFF file");
    const std::uint16_t version = o.u16(hdr + 2);
    bool big = false;
    std::uint64_t ifd = 0;
    if (version == 42) {
        ifd = o.u32(hdr + 4);
    } else if (version == 43) {
        if (file.size() < 16) throw SourceError(path + ": truncated BigTIFF header");
        if (o.u16(hdr + 4) != 8 || o.u16(hdr + 6) != 0) throw SourceError(path + ": bad BigTIFF header");
        big = true;
        ifd = o.u64(hdr + 8);
    } else {
        throw SourceError(path + ": not a TIFF file (bad version)");
    }
    layout.bigTiff = big;
    layout.bigEndian = o.big;
    layout.fileSize = file.size();

    // Find the first full-resolution image: not an overview, not a mask.
    Parser p(file, o, big);
    std::set<std::uint64_t> seen;
    std::map<std::uint16_t, Entry> tags;
    bool found = false;
    for (int k = 0; k < kMaxIfds && ifd != 0; ++k) {
        if (!seen.insert(ifd).second) break;   // a loop
        std::uint64_t next = 0;
        auto entries = p.readIfd(ifd, next);
        std::uint64_t subfile = 0;
        if (auto it = entries.find(kNewSubfileType); it != entries.end() && it->second.count >= 1)
            subfile = p.ints(it->second)[0];
        bool reduced = (subfile & 1) != 0 || (subfile & 4) != 0;
        if (auto it = entries.find(kSubfileType); it != entries.end() && it->second.count >= 1)
            reduced = reduced || p.ints(it->second)[0] == 2;
        if (!reduced) {
            tags = std::move(entries);
            layout.ifdOffset = ifd;
            found = true;
            break;
        }
        ifd = next;
    }
    if (!found) throw SourceError(path + ": no full-resolution image");

    auto has = [&](std::uint16_t t) { return tags.count(t) != 0 && tags.at(t).count > 0; };
    auto scalar = [&](std::uint16_t t, std::uint64_t def) -> std::uint64_t {
        if (!has(t)) return def;
        return p.ints(tags.at(t))[0];
    };
    auto required = [&](std::uint16_t t, const char* name) -> std::uint64_t {
        if (!has(t)) throw SourceError(path + ": missing tag " + name);
        return p.ints(tags.at(t))[0];
    };

    const std::uint64_t width = required(kImageWidth, "ImageWidth");
    const std::uint64_t height = required(kImageLength, "ImageLength");
    if (width == 0 || height == 0 || width > 0xFFFFFFFFu || height > 0xFFFFFFFFu)
        throw SourceError(path + ": bad image size");
    const std::uint64_t spp = scalar(kSamplesPerPixel, 1);
    if (spp != 1) throw SourceError(path + ": " + std::to_string(spp) + " samples per pixel (only 1 band is supported)");
    const std::uint64_t bits = scalar(kBitsPerSample, 1);
    const std::uint64_t format = scalar(kSampleFormat, 1);
    const std::uint64_t compression = scalar(kCompression, 1);
    const std::uint64_t predictor = scalar(kPredictor, 1);

    SampleType st;
    if (format == 1 && bits == 8) st = SampleType::U8, convert = convertRow<std::uint8_t>;
    else if (format == 1 && bits == 16) st = SampleType::U16, convert = convertRow<std::uint16_t>;
    else if (format == 2 && bits == 16) st = SampleType::I16, convert = convertRow<std::int16_t>;
    else if (format == 1 && bits == 32) st = SampleType::U32, convert = convertRow<std::uint32_t>;
    else if (format == 2 && bits == 32) st = SampleType::I32, convert = convertRow<std::int32_t>;
    else if (format == 3 && bits == 32) st = SampleType::F32, convert = convertRow<float>;
    else if (format == 3 && bits == 64) st = SampleType::F64, convert = convertRow<double>;
    else
        throw SourceError(path + ": unsupported sample type (format " + std::to_string(format) + ", " +
                          std::to_string(bits) + " bits)");
    bytesPerSample = unsigned(bits / 8);

    if (compression == 50000 && !tiffcodec::zstdAvailable())
        throw SourceError(path + ": compression 50000 (ZSTD): this build has no ZSTD support (build with PYRE_WITH_ZSTD and libzstd)");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 50000)
        throw SourceError(path + ": unsupported compression " + std::to_string(compression));
    if (predictor != 1 && predictor != 2 && predictor != 3)
        throw SourceError(path + ": unsupported predictor " + std::to_string(predictor));
    if (predictor == 3 && format != 3) throw SourceError(path + ": predictor 3 needs floating-point samples");

    std::uint64_t bw, bh;
    const bool tiled = has(kTileWidth) || has(kTileLength) || has(kTileOffsets);
    if (tiled) {
        bw = required(kTileWidth, "TileWidth");
        bh = required(kTileLength, "TileLength");
    } else {
        bw = width;
        bh = std::min<std::uint64_t>(scalar(kRowsPerStrip, height), height);
    }
    if (bw == 0 || bh == 0 || bw > 0xFFFFFFFFu || bh > 0xFFFFFFFFu) throw SourceError(path + ": bad block size");
    if (bw * bh > kMaxBlockBytes / bytesPerSample) throw SourceError(path + ": blocks are too large");
    const std::uint64_t across = (width + bw - 1) / bw, down = (height + bh - 1) / bh;
    if (across * down > kMaxBlocks) throw SourceError(path + ": too many blocks");

    const std::uint16_t offTag = tiled ? kTileOffsets : kStripOffsets;
    const std::uint16_t cntTag = tiled ? kTileByteCounts : kStripByteCounts;
    if (!has(offTag)) throw SourceError(path + ": missing block offsets");
    if (!has(cntTag)) throw SourceError(path + ": missing block byte counts");
    offsets = p.ints(tags.at(offTag));
    counts = p.ints(tags.at(cntTag));
    const std::uint64_t nBlocks = across * down;
    if (offsets.size() < nBlocks || counts.size() < nBlocks) throw SourceError(path + ": too few block offsets");
    offsets.resize(std::size_t(nBlocks));
    counts.resize(std::size_t(nBlocks));
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (counts[i] == 0) continue;   // a sparse block
        if (!file.inside(offsets[i], counts[i])) throw SourceError(path + ": block data out of the file (truncated?)");
    }

    layout.tiled = tiled;
    layout.blockWidth = std::uint32_t(bw);
    layout.blockHeight = std::uint32_t(bh);
    layout.blocksAcross = std::uint32_t(across);
    layout.blocksDown = std::uint32_t(down);
    layout.compression = std::uint16_t(compression);
    layout.predictor = std::uint16_t(predictor);
    layout.bitsPerSample = std::uint16_t(bits);

    info.path = path;
    info.width = std::uint32_t(width);
    info.height = std::uint32_t(height);
    info.sample = st;

    // ── GeoTIFF ──
    Georef& g = info.georef;
    if (has(kGeoKeyDirectory)) {
        const auto keys = p.ints(tags.at(kGeoKeyDirectory));
        std::vector<double> dparams;
        std::string aparams;
        if (has(kGeoDoubleParams)) dparams = p.doubles(tags.at(kGeoDoubleParams));
        if (has(kGeoAsciiParams)) {
            const auto b = p.bytes(tags.at(kGeoAsciiParams));
            aparams.assign(b.begin(), b.end());
        }
        std::map<std::uint16_t, std::uint64_t> ikeys;
        std::map<std::uint16_t, std::string> skeys;
        if (keys.size() >= 4) {
            const std::uint64_t n = std::min<std::uint64_t>(keys[3], (keys.size() - 4) / 4);
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto id = std::uint16_t(keys[4 + 4 * i]);
                const std::uint64_t loc = keys[5 + 4 * i], cnt = keys[6 + 4 * i], val = keys[7 + 4 * i];
                if (loc == 0) {
                    ikeys[id] = val;
                } else if (loc == kGeoAsciiParams) {
                    if (val <= aparams.size() && cnt <= aparams.size() - val) {
                        std::string s = aparams.substr(std::size_t(val), std::size_t(cnt));
                        while (!s.empty() && (s.back() == '|' || s.back() == '\0')) s.pop_back();
                        skeys[id] = s;
                    }
                } else if (loc == kGeoDoubleParams) {
                    (void)dparams;   // no double key is needed here
                } else if (loc == kGeoKeyDirectory) {
                    if (cnt >= 1 && val < keys.size()) ikeys[id] = keys[std::size_t(val)];
                }
            }
        }
        auto ikey = [&](std::uint16_t id) -> std::optional<std::uint64_t> {
            if (auto it = ikeys.find(id); it != ikeys.end()) return it->second;
            return std::nullopt;
        };
        if (auto m = ikey(1024)) g.model = (*m >= 1 && *m <= 3) ? ModelType(*m) : ModelType::Unknown;
        if (auto r = ikey(1025)) g.rasterType = *r == 2 ? RasterType::PixelIsPoint : RasterType::PixelIsArea;
        auto code = [](std::optional<std::uint64_t> v) -> std::optional<std::uint32_t> {
            if (!v || *v == 0 || *v == 32767 || *v > 0xFFFFFFFFu) return std::nullopt;
            return std::uint32_t(*v);
        };
        const auto projected = code(ikey(3072)), geographic = code(ikey(2048));
        if (g.model == ModelType::Projected)
            g.epsg = projected ? projected : std::nullopt;
        else if (g.model == ModelType::Geographic || g.model == ModelType::Geocentric)
            g.epsg = geographic;
        else
            g.epsg = projected ? projected : geographic;
        g.verticalEpsg = code(ikey(4096));
        if (!g.epsg) {
            for (std::uint16_t id : {std::uint16_t(1026), std::uint16_t(3073), std::uint16_t(2049)})
                if (auto it = skeys.find(id); it != skeys.end() && !it->second.empty()) {
                    g.wkt = it->second;
                    break;
                }
        }
    }
    if (has(kModelPixelScale) && has(kModelTiepoint)) {
        const auto s = p.doubles(tags.at(kModelPixelScale));
        const auto t = p.doubles(tags.at(kModelTiepoint));
        if (s.size() < 2 || t.size() < 6) throw SourceError(path + ": bad ModelPixelScale or ModelTiepoint");
        g.spacingX = s[0];
        g.spacingY = s[1];
        g.originX = t[3] - t[0] * s[0];
        g.originY = t[4] + t[1] * s[1];
    } else if (has(kModelTransformation)) {
        const auto m = p.doubles(tags.at(kModelTransformation));
        if (m.size() < 16) throw SourceError(path + ": bad ModelTransformation");
        if (m[1] != 0 || m[4] != 0) throw SourceError(path + ": rotated rasters are not supported");
        g.spacingX = m[0];
        g.spacingY = -m[5];
        g.originX = m[3];
        g.originY = m[7];
    }
    if (!std::isfinite(g.spacingX) || !std::isfinite(g.spacingY) || !std::isfinite(g.originX) ||
        !std::isfinite(g.originY) || g.spacingX == 0 || g.spacingY == 0)
        throw SourceError(path + ": bad georeference");

    if (has(kGdalNoData)) info.noData = parseNoData(p.ascii(tags.at(kGdalNoData)));
    if (has(kGdalMetadata)) g.verticalUnit = unitFromGdalMetadata(p.ascii(tags.at(kGdalMetadata)));
}

BlockPtr TiffSource::Impl::decodeBlock(std::uint64_t b) const {
    const std::uint64_t cnt = counts[std::size_t(b)];
    if (cnt == 0) return nullptr;   // sparse
    const std::uint64_t by = b / layout.blocksAcross;
    // A strip image's last strip may hold fewer rows; tiles are always whole.
    std::uint64_t rows = layout.blockHeight;
    if (!layout.tiled) rows = std::min<std::uint64_t>(rows, info.height - by * layout.blockHeight);
    const std::size_t rowSamples = layout.blockWidth;
    const std::size_t outSize = std::size_t(rows * rowSamples * bytesPerSample);

    // Deflate expands at most about 1032:1, LZW less; a larger ratio means a
    // damaged header, and we stop before the allocation. ZSTD can expand far
    // more (a block of one value); its frame says its size, checked below.
    if (layout.compression != 1 && layout.compression != 50000 && outSize / 2048 > cnt + 32)
        throw SourceError(file.path() + ": block size does not match its compressed size");

    auto blk = std::make_shared<Block>();
    blk->data.reset(new std::uint8_t[std::size_t(layout.blockWidth) * layout.blockHeight * bytesPerSample]);
    blk->size = outSize;
    std::uint8_t* out = blk->data.get();

    if (layout.compression == 1) {
        if (cnt < outSize) throw SourceError(file.path() + ": block too short");
        file.read(offsets[std::size_t(b)], out, outSize);
    } else {
        thread_local std::vector<std::uint8_t> packed;
        if (packed.size() < cnt) packed.resize(std::size_t(cnt));
        file.read(offsets[std::size_t(b)], packed.data(), cnt);
        if (layout.compression == 5) {
            tiffcodec::lzwDecodeExact(packed.data(), std::size_t(cnt), out, outSize);
        } else if (layout.compression == 50000) {
            const std::uint64_t said = tiffcodec::zstdFrameSize(packed.data(), std::size_t(cnt));
            if (said != 0 && said != outSize) throw SourceError(file.path() + ": block size does not match its ZSTD frame");
            tiffcodec::zstdDecompressExact(packed.data(), std::size_t(cnt), out, outSize);
        } else {
            tiffcodec::inflateExact(packed.data(), std::size_t(cnt), out, outSize);
        }
        if (packed.size() > (std::size_t(64) << 20)) std::vector<std::uint8_t>().swap(packed);
    }

    if (layout.predictor == 3) {
        tiffcodec::undoFloatingPoint(out, rowSamples, std::size_t(rows), bytesPerSample);
    } else {
        const bool swap = layout.bigEndian != (std::endian::native == std::endian::big);
        if (swap) tiffcodec::byteSwap(out, std::size_t(rows) * rowSamples, bytesPerSample);
        if (layout.predictor == 2) tiffcodec::undoHorizontal(out, rowSamples, std::size_t(rows), bytesPerSample);
    }
    return blk;
}

BlockPtr TiffSource::Impl::getBlock(std::uint64_t b) const {
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (auto it = index.find(b); it != index.end()) {
            lru.splice(lru.begin(), lru, it->second);
            return it->second->second;
        }
    }
    BlockPtr blk = decodeBlock(b);   // outside the lock
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cacheLimit == 0) return blk;
    if (auto it = index.find(b); it != index.end()) return it->second->second;   // another thread was first
    lru.emplace_front(b, blk);
    index[b] = lru.begin();
    while (lru.size() > cacheLimit) {
        index.erase(lru.back().first);
        lru.pop_back();
    }
    return blk;
}

// ── TiffSource ───────────────────────────────────────────────────────────────

TiffSource::TiffSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TiffSource::~TiffSource() = default;

std::unique_ptr<TiffSource> TiffSource::open(const std::string& path, std::size_t cacheBlocks) {
    auto impl = std::make_unique<Impl>(path);
    try {
        impl->parse();
    } catch (const SourceError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw SourceError(path + ": out of memory while reading the header (damaged file?)");
    } catch (const std::exception& e) {
        throw SourceError(path + ": " + e.what());
    }
    impl->cacheLimit = cacheBlocks;
    return std::unique_ptr<TiffSource>(new TiffSource(std::move(impl)));
}

const SourceInfo& TiffSource::info() const { return impl_->info; }
const TiffLayout& TiffSource::layout() const { return impl_->layout; }

void TiffSource::setCacheLimit(std::size_t blocks) const {
    std::lock_guard<std::mutex> lock(impl_->cacheMutex);
    impl_->cacheLimit = blocks;
    while (impl_->lru.size() > blocks) {
        impl_->index.erase(impl_->lru.back().first);
        impl_->lru.pop_back();
    }
}

void TiffSource::readWindow(std::uint32_t col0, std::uint32_t row0, std::uint32_t width, std::uint32_t height,
                            double* out) const {
    const Impl& m = *impl_;
    const SourceInfo& in = m.info;
    if (std::uint64_t(col0) + width > in.width || std::uint64_t(row0) + height > in.height)
        throw SourceError(in.path + ": window outside the raster");
    if (width == 0 || height == 0) return;
    if (!out) throw SourceError("readWindow: null output");

    const std::uint64_t bw = m.layout.blockWidth, bh = m.layout.blockHeight;
    const std::uint64_t c1 = std::uint64_t(col0) + width, r1 = std::uint64_t(row0) + height;
    const double fill = in.noData ? *in.noData : 0.0;
    const std::size_t bps = m.bytesPerSample;

    for (std::uint64_t by = row0 / bh; by * bh < r1; ++by) {
        const std::uint64_t ry0 = std::max<std::uint64_t>(row0, by * bh);
        const std::uint64_t ry1 = std::min<std::uint64_t>(r1, (by + 1) * bh);
        for (std::uint64_t bx = col0 / bw; bx * bw < c1; ++bx) {
            const std::uint64_t cx0 = std::max<std::uint64_t>(col0, bx * bw);
            const std::uint64_t cx1 = std::min<std::uint64_t>(c1, (bx + 1) * bw);
            const std::size_t n = std::size_t(cx1 - cx0);
            BlockPtr blk;
            try {
                blk = m.getBlock(by * m.layout.blocksAcross + bx);
            } catch (const SourceError&) {
                throw;
            } catch (const std::bad_alloc&) {
                throw SourceError(in.path + ": out of memory while decoding a block");
            }
            for (std::uint64_t r = ry0; r < ry1; ++r) {
                double* dst = out + std::size_t(r - row0) * width + std::size_t(cx0 - col0);
                if (!blk) {
                    std::fill(dst, dst + n, fill);
                    continue;
                }
                const std::size_t off = std::size_t(((r - by * bh) * bw + (cx0 - bx * bw)) * bps);
                if (off + n * bps > blk->size) throw SourceError(in.path + ": block too short");
                m.convert(blk->data.get() + off, dst, n);
            }
        }
    }
}

}  // namespace pyre::tool
