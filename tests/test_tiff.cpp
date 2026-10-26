// Tests of the GeoTIFF reader and writer (tools/pyre/tiff.hpp).
//
// The hand-built files here use their own small TIFF encoder (byte order,
// predictors, LZW, Deflate), written apart from the reader, so that the two
// do not share a mistake.

#include <unistd.h>
#include <zlib.h>
#ifdef PYRE_HAVE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "synth.hpp"
#include "tiff.hpp"

using namespace pyre::tool;
namespace fs = std::filesystem;

namespace {

// ── Temporary files ──────────────────────────────────────────────────────────

// One folder for each run; it is removed at exit (when empty).
struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / ("pyre_test_tiff_" + std::to_string(::getpid()))) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

fs::path tempDir() {
    static const TempDir dir;
    return dir.path;
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name) : path((tempDir() / name).string()) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
};

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool sameValue(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

// ── A small independent TIFF encoder ─────────────────────────────────────────

unsigned sampleBytes(SampleType t) {
    switch (t) {
        case SampleType::U8: return 1;
        case SampleType::I16: case SampleType::U16: return 2;
        case SampleType::I32: case SampleType::U32: case SampleType::F32: return 4;
        case SampleType::F64: return 8;
    }
    return 0;
}
std::uint16_t sampleFormat(SampleType t) {
    switch (t) {
        case SampleType::U8: case SampleType::U16: case SampleType::U32: return 1;
        case SampleType::I16: case SampleType::I32: return 2;
        default: return 3;
    }
}
const char* sampleName(SampleType t) {
    static const char* n[] = {"u8", "i16", "u16", "i32", "u32", "f32", "f64"};
    return n[int(t)];
}

/// The value's bits as an unsigned integer of the sample's size.
std::uint64_t valueBits(SampleType t, double v) {
    switch (t) {
        case SampleType::U8: return std::uint8_t(v);
        case SampleType::I16: return std::uint16_t(std::int16_t(v));
        case SampleType::U16: return std::uint16_t(v);
        case SampleType::I32: return std::uint32_t(std::int32_t(v));
        case SampleType::U32: return std::uint32_t(v);
        case SampleType::F32: return std::bit_cast<std::uint32_t>(float(v));
        case SampleType::F64: return std::bit_cast<std::uint64_t>(v);
    }
    return 0;
}

void putN(std::vector<std::uint8_t>& b, std::uint64_t v, unsigned n, bool be) {
    for (unsigned i = 0; i < n; ++i) b.push_back(std::uint8_t(v >> (8 * (be ? n - 1 - i : i))));
}

std::vector<std::uint8_t> zlibCompress(const std::vector<std::uint8_t>& in) {
    uLongf len = compressBound(uLong(in.size()));
    std::vector<std::uint8_t> out(len);
    compress2(out.data(), &len, in.data(), uLong(in.size()), 9);
    out.resize(len);
    return out;
}

#ifdef PYRE_HAVE_ZSTD
std::vector<std::uint8_t> zstdCompress(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
    out.resize(ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 9));
    return out;
}
#endif

/// The compressions to test: ZSTD too when the build has it.
std::vector<std::uint16_t> compressions() {
    std::vector<std::uint16_t> c = {1, 5, 8, 32946};
#ifdef PYRE_HAVE_ZSTD
    c.push_back(50000);
#endif
    return c;
}

/// TIFF LZW as libtiff writes it: a clear code first; codes MSB first; the
/// code width grows when the next free code passes 511, 1023, 2047; a clear
/// code when the next free code reaches 4094; EOI at the end.
std::vector<std::uint8_t> lzwCompress(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    std::uint64_t acc = 0;
    int nacc = 0;
    unsigned bits = 9;
    auto put = [&](unsigned code) {
        acc = (acc << bits) | code;
        nacc += int(bits);
        while (nacc >= 8) {
            out.push_back(std::uint8_t(acc >> (nacc - 8)));
            nacc -= 8;
        }
    };
    std::map<std::pair<unsigned, unsigned>, unsigned> table;
    unsigned next = 258;
    auto grow = [&] {
        if (next == 4094) {
            put(256);
            table.clear();
            next = 258;
            bits = 9;
        } else if (next > (1u << bits) - 1) {
            ++bits;
        }
    };
    put(256);
    if (!in.empty()) {
        unsigned ent = in[0];
        for (std::size_t i = 1; i < in.size(); ++i) {
            const unsigned c = in[i];
            if (auto it = table.find({ent, c}); it != table.end()) {
                ent = it->second;
                continue;
            }
            put(ent);
            table[{ent, c}] = next++;
            grow();
            ent = c;
        }
        put(ent);
        ++next;
        grow();
    }
    put(257);
    if (nacc > 0) out.push_back(std::uint8_t(acc << (8 - nacc)));
    return out;
}

struct TTag {
    std::uint16_t tag = 0, type = 0;
    std::vector<std::uint64_t> ints;   // SHORT, LONG, LONG8
    std::vector<double> dbls;          // DOUBLE
    std::string str;                   // ASCII (without the NUL)
};
TTag tShort(std::uint16_t tag, std::vector<std::uint64_t> v) { return {tag, 3, std::move(v), {}, {}}; }
TTag tLong(std::uint16_t tag, std::vector<std::uint64_t> v) { return {tag, 4, std::move(v), {}, {}}; }
TTag tDouble(std::uint16_t tag, std::vector<double> v) { return {tag, 12, {}, std::move(v), {}}; }
TTag tAscii(std::uint16_t tag, std::string s) { return {tag, 2, {}, {}, std::move(s)}; }

struct TIfd {
    std::vector<TTag> tags;
    std::vector<std::vector<std::uint8_t>> blocks;   // an empty block is sparse (offset 0, count 0)
    bool tiled = true;
};

/// Lays out a whole file: the header, the blocks, then the IFDs (each one
/// followed by its values).
std::vector<std::uint8_t> assemble(bool be, bool big, std::vector<TIfd> ifds) {
    std::vector<std::uint8_t> f;
    f.push_back(be ? 'M' : 'I');
    f.push_back(be ? 'M' : 'I');
    putN(f, big ? 43 : 42, 2, be);
    if (big) {
        putN(f, 8, 2, be);
        putN(f, 0, 2, be);
        putN(f, 0, 8, be);
    } else {
        putN(f, 0, 4, be);
    }
    for (auto& d : ifds) {
        std::vector<std::uint64_t> offs, counts;
        for (auto& b : d.blocks) {
            if (f.size() % 2) f.push_back(0);
            offs.push_back(b.empty() ? 0 : f.size());
            counts.push_back(b.size());
            f.insert(f.end(), b.begin(), b.end());
        }
        const std::uint16_t lt = big ? 16 : 4;
        d.tags.push_back({std::uint16_t(d.tiled ? 324 : 273), lt, offs, {}, {}});
        d.tags.push_back({std::uint16_t(d.tiled ? 325 : 279), lt, counts, {}, {}});
        std::sort(d.tags.begin(), d.tags.end(), [](const TTag& a, const TTag& b) { return a.tag < b.tag; });
    }
    if (f.size() % 2) f.push_back(0);
    const unsigned cs = big ? 8 : 2, es = big ? 20 : 12, ns = big ? 8 : 4, field = big ? 8 : 4;
    auto serialize = [&](const TIfd& d, std::uint64_t base, std::uint64_t next) {
        std::vector<std::uint8_t> out, extra;
        const std::uint64_t extraBase = base + cs + d.tags.size() * es + ns;
        putN(out, d.tags.size(), cs, be);
        for (const TTag& t : d.tags) {
            std::vector<std::uint8_t> data;
            std::uint64_t count = 0;
            if (t.type == 2) {
                data.assign(t.str.begin(), t.str.end());
                data.push_back(0);
                count = data.size();
            } else if (t.type == 12) {
                for (double v : t.dbls) putN(data, std::bit_cast<std::uint64_t>(v), 8, be);
                count = t.dbls.size();
            } else {
                const unsigned n = t.type == 3 ? 2 : t.type == 4 ? 4 : 8;
                for (auto v : t.ints) putN(data, v, n, be);
                count = t.ints.size();
            }
            putN(out, t.tag, 2, be);
            putN(out, t.type, 2, be);
            putN(out, count, big ? 8 : 4, be);
            if (data.size() <= field) {
                data.resize(field, 0);
                out.insert(out.end(), data.begin(), data.end());
            } else {
                putN(out, extraBase + extra.size(), field, be);
                extra.insert(extra.end(), data.begin(), data.end());
                if (extra.size() % 2) extra.push_back(0);
            }
        }
        putN(out, next, ns, be);
        out.insert(out.end(), extra.begin(), extra.end());
        return out;
    };
    std::vector<std::uint64_t> bases;
    std::uint64_t pos = f.size();
    for (const auto& d : ifds) {
        bases.push_back(pos);
        pos += serialize(d, 0, 0).size();
    }
    for (std::size_t k = 0; k < ifds.size(); ++k) {
        const auto bytes = serialize(ifds[k], bases[k], k + 1 < ifds.size() ? bases[k + 1] : 0);
        f.insert(f.end(), bytes.begin(), bytes.end());
    }
    // The first IFD's offset.
    std::vector<std::uint8_t> p;
    putN(p, bases.empty() ? 0 : bases[0], big ? 8 : 4, be);
    std::copy(p.begin(), p.end(), f.begin() + (big ? 8 : 4));
    return f;
}

struct Spec {
    SampleType type = SampleType::F32;
    std::uint16_t compression = 1, predictor = 1;
    bool tiled = false, bigEndian = false, bigTiff = false;
    std::uint32_t width = 37, height = 23, blockW = 16, blockH = 16, rowsPerStrip = 3;

    std::string name() const {
        return std::string(sampleName(type)) + " c" + std::to_string(compression) + " p" + std::to_string(predictor) +
               (tiled ? " tiles" : " strips") + (bigEndian ? " MM" : " II") + (bigTiff ? " big" : " classic");
    }
};

std::vector<double> testValues(SampleType t, std::uint32_t w, std::uint32_t h, std::uint64_t seed) {
    synth::Rng r(seed);
    std::vector<double> v(std::size_t(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::int64_t k = std::int64_t(x) * 311 + std::int64_t(y) * 977 + std::int64_t(r.next() % 17);
            double d = 0;
            switch (t) {
                case SampleType::U8: d = double(k % 256); break;
                case SampleType::I16: d = double(k % 65536 - 32768); break;
                case SampleType::U16: d = double(k % 65536); break;
                case SampleType::I32: d = double((k * 7919) % 4000000000ll - 2000000000ll); break;
                case SampleType::U32: d = double((k * 7919) % 4294967296ll); break;
                case SampleType::F32:
                    d = double(float(1000 * std::sin(x / 5.0) + 37.25 * std::cos(y / 3.0) + double(k % 17) * 0.01));
                    break;
                case SampleType::F64: d = 1000 * std::sin(x / 5.0) + 1e-7 * double(x * y) + double(k % 17) * 0.01; break;
            }
            v[std::size_t(y) * w + x] = d;
        }
    return v;
}

std::vector<std::uint8_t> encodeBlock(const Spec& s, const std::vector<double>& vals, std::uint32_t bx,
                                      std::uint32_t by) {
    const unsigned n = sampleBytes(s.type);
    const std::uint32_t bw = s.tiled ? s.blockW : s.width;
    const std::uint32_t bh = s.tiled ? s.blockH : s.rowsPerStrip;
    const std::uint32_t rows = s.tiled ? bh : std::min(bh, s.height - by * bh);
    const std::uint64_t mask = n == 8 ? ~std::uint64_t(0) : (std::uint64_t(1) << (8 * n)) - 1;
    std::vector<std::uint8_t> raw;
    std::vector<std::uint64_t> row(bw);
    for (std::uint32_t r = 0; r < rows; ++r) {
        const std::uint32_t y = by * bh + r;
        for (std::uint32_t c = 0; c < bw; ++c) {
            const std::uint32_t x = bx * bw + c;
            row[c] = (x < s.width && y < s.height) ? valueBits(s.type, vals[std::size_t(y) * s.width + x]) : 0;
        }
        if (s.predictor == 2)
            for (std::uint32_t c = bw - 1; c > 0; --c) row[c] = (row[c] - row[c - 1]) & mask;
        if (s.predictor == 3) {
            std::vector<std::uint8_t> planes(std::size_t(bw) * n);
            for (unsigned k = 0; k < n; ++k)
                for (std::uint32_t c = 0; c < bw; ++c) planes[k * bw + c] = std::uint8_t(row[c] >> (8 * (n - 1 - k)));
            for (std::size_t j = planes.size() - 1; j > 0; --j) planes[j] = std::uint8_t(planes[j] - planes[j - 1]);
            raw.insert(raw.end(), planes.begin(), planes.end());
        } else {
            for (std::uint32_t c = 0; c < bw; ++c) putN(raw, row[c], n, s.bigEndian);
        }
    }
    if (s.compression == 5) return lzwCompress(raw);
    if (s.compression == 8 || s.compression == 32946) return zlibCompress(raw);
#ifdef PYRE_HAVE_ZSTD
    if (s.compression == 50000) return zstdCompress(raw);
#endif
    return raw;
}

std::vector<TTag> imageTags(const Spec& s) {
    std::vector<TTag> t = {
        tShort(256, {s.width}),
        tLong(257, {s.height}),
        tShort(258, {8 * sampleBytes(s.type)}),
        tShort(259, {s.compression}),
        tShort(262, {1}),
        tShort(277, {1}),
        tShort(339, {sampleFormat(s.type)}),
    };
    if (s.predictor != 1) t.push_back(tShort(317, {s.predictor}));
    if (s.tiled) {
        t.push_back(tShort(322, {s.blockW}));
        t.push_back(tShort(323, {s.blockH}));
    } else {
        t.push_back(tLong(278, {s.rowsPerStrip}));
    }
    return t;
}

std::vector<TTag> geoTags() {
    return {
        tDouble(33550, {0.5, 0.25, 0}),
        tDouble(33922, {0, 0, 0, 440000, 4470000, 0}),
        tShort(34735, {1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 25830}),
        tAscii(42113, "-9999"),
    };
}

TIfd imageIfd(const Spec& s, const std::vector<double>& vals, int sparseBlock = -1) {
    TIfd d;
    d.tiled = s.tiled;
    d.tags = imageTags(s);
    const std::uint32_t across = s.tiled ? (s.width + s.blockW - 1) / s.blockW : 1;
    const std::uint32_t down = s.tiled ? (s.height + s.blockH - 1) / s.blockH : (s.height + s.rowsPerStrip - 1) / s.rowsPerStrip;
    for (std::uint32_t by = 0; by < down; ++by)
        for (std::uint32_t bx = 0; bx < across; ++bx) {
            if (int(by * across + bx) == sparseBlock)
                d.blocks.emplace_back();
            else
                d.blocks.push_back(encodeBlock(s, vals, bx, by));
        }
    return d;
}

std::vector<std::uint8_t> buildFile(const Spec& s, const std::vector<double>& vals) {
    TIfd d = imageIfd(s, vals);
    for (auto& t : geoTags()) d.tags.push_back(t);
    return assemble(s.bigEndian, s.bigTiff, {d});
}

/// Reads the whole raster and a few windows; true when all equal `vals`.
bool readsBack(const Source& src, const std::vector<double>& vals) {
    const auto& in = src.info();
    std::vector<double> all(std::size_t(in.width) * in.height);
    src.readWindow(0, 0, in.width, in.height, all.data());
    for (std::size_t i = 0; i < all.size(); ++i)
        if (!sameValue(all[i], vals[i])) return false;
    synth::Rng r(in.width * 31 + in.height);
    for (int k = 0; k < 20; ++k) {
        const std::uint32_t c0 = std::uint32_t(r.next() % in.width), r0 = std::uint32_t(r.next() % in.height);
        const std::uint32_t w = 1 + std::uint32_t(r.next() % (in.width - c0)), h = 1 + std::uint32_t(r.next() % (in.height - r0));
        std::vector<double> win(std::size_t(w) * h);
        src.readWindow(c0, r0, w, h, win.data());
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                if (!sameValue(win[std::size_t(y) * w + x], vals[std::size_t(r0 + y) * in.width + c0 + x])) return false;
    }
    return true;
}

template <class F>
bool throwsSourceError(F&& f) {
    try {
        f();
    } catch (const SourceError&) {
        return true;
    }
    return false;
}

/// Opens a file and reads it; a damaged file may read or throw SourceError,
/// nothing else. Returns 1 when it read, 0 when it threw SourceError.
int openAndRead(const std::string& path) {
    try {
        auto src = TiffSource::open(path);
        const auto& in = src->info();
        if (std::uint64_t(in.width) * in.height <= (1u << 20)) {
            std::vector<double> all(std::size_t(in.width) * in.height);
            src->readWindow(0, 0, in.width, in.height, all.data());
        } else {
            std::vector<double> w(64 * 64);
            src->readWindow(0, 0, std::min(in.width, 64u), std::min(in.height, 64u), w.data());
            src->readWindow(in.width - std::min(in.width, 64u), in.height - std::min(in.height, 64u),
                            std::min(in.width, 64u), std::min(in.height, 64u), w.data());
        }
        return 1;
    } catch (const SourceError&) {
        return 0;
    }
}

std::vector<float> writerData(std::uint32_t w, std::uint32_t h) {
    std::vector<float> d(std::size_t(w) * h);
    synth::Rng r(7);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            d[std::size_t(y) * w + x] = float(600 + 80 * std::sin(x / 40.0) * std::cos(y / 25.0) + 0.37 * x - 0.11 * y +
                                              double(r.next() % 1000) * 0.001);
    d[5] = -32767.0f;
    d[std::size_t(w) * h - 1] = -32767.0f;
    // Special values in column 30, rows 10 to 13 (the rasters are larger).
    d[std::size_t(w) * 10 + 30] = std::numeric_limits<float>::quiet_NaN();
    d[std::size_t(w) * 11 + 30] = std::numeric_limits<float>::infinity();
    d[std::size_t(w) * 12 + 30] = -0.0f;
    d[std::size_t(w) * 13 + 30] = 1e-40f;   // a denormal
    return d;
}

}  // namespace

// ── The writer, then the reader ──────────────────────────────────────────────

TEST(writer_roundtrip) {
    const std::uint32_t w = 600, h = 300;
    const auto data = writerData(w, h);
    Georef g;
    g.model = ModelType::Geographic;
    g.epsg = 4326;
    g.verticalEpsg = 5782;
    g.verticalUnit = "metre";
    g.rasterType = RasterType::PixelIsPoint;
    g.originX = -3.123456789012345;
    g.originY = 40.987654321098765;
    g.spacingX = 0.000018;
    g.spacingY = 0.000018;
    TempFile f("writer.tif");
    writeGeoTiff(f.path, w, h, data.data(), g, -32767.0);

    auto src = TiffSource::open(f.path);
    const auto& in = src->info();
    CHECK(in.width == w);
    CHECK(in.height == h);
    CHECK(in.sample == SampleType::F32);
    CHECK(in.noData && *in.noData == -32767.0);
    CHECK(in.georef.model == ModelType::Geographic);
    CHECK(in.georef.epsg && *in.georef.epsg == 4326);
    CHECK(in.georef.verticalEpsg && *in.georef.verticalEpsg == 5782);
    CHECK(in.georef.verticalUnit == "metre");
    CHECK(in.georef.wkt.empty());
    CHECK(in.georef.rasterType == RasterType::PixelIsPoint);
    CHECK(in.georef.originX == g.originX);
    CHECK(in.georef.originY == g.originY);
    CHECK(in.georef.spacingX == g.spacingX);
    CHECK(in.georef.spacingY == g.spacingY);
    const auto& lay = src->layout();
    CHECK(!lay.bigTiff);
    CHECK(lay.tiled);
    CHECK(lay.blockWidth == 256 && lay.blockHeight == 256);
    CHECK(lay.blocksAcross == 3 && lay.blocksDown == 2);
    CHECK(lay.compression == 8);
    CHECK(lay.predictor == 3);

    std::vector<double> all(std::size_t(w) * h);
    src->readWindow(0, 0, w, h, all.data());
    std::size_t bad = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const float back = float(all[i]);
        if (std::bit_cast<std::uint32_t>(back) != std::bit_cast<std::uint32_t>(data[i])) ++bad;
    }
    CHECK(bad == 0);
    CHECK(in.isNoData(all[5]));
    CHECK(in.isNoData(all[std::size_t(w) * 10 + 30]));
    CHECK(!in.isNoData(all[6]));
}

TEST(writer_projected_citation) {
    const std::uint32_t w = 3, h = 2;
    const float data[6] = {1, 2, 3, 4, 5, 6};
    Georef g;
    g.model = ModelType::Projected;
    g.wkt = "ETRS89 / custom transverse mercator";
    g.originX = 440000;
    g.originY = 4470000;
    g.spacingX = 2;
    g.spacingY = 2;
    TempFile f("writer2.tif");
    writeGeoTiff(f.path, w, h, data, g, std::nullopt);
    auto src = TiffSource::open(f.path);
    const auto& in = src->info();
    CHECK(in.georef.model == ModelType::Projected);
    CHECK(!in.georef.epsg);
    CHECK(in.georef.wkt == g.wkt);
    CHECK(in.georef.rasterType == RasterType::PixelIsArea);
    CHECK(!in.noData);
    CHECK(in.georef.verticalUnit.empty());
    double out[6];
    src->readWindow(0, 0, 3, 2, out);
    for (int i = 0; i < 6; ++i) CHECK(out[i] == data[i]);

    // EPSG of a projected CRS.
    g.wkt.clear();
    g.epsg = 25830;
    writeGeoTiff(f.path, w, h, data, g, std::numeric_limits<double>::quiet_NaN());
    auto src2 = TiffSource::open(f.path);
    CHECK(src2->info().georef.epsg && *src2->info().georef.epsg == 25830);
    CHECK(src2->info().noData && std::isnan(*src2->info().noData));
}

TEST(windows_cross_tile_edges) {
    const std::uint32_t w = 600, h = 300;
    const auto data = writerData(w, h);
    Georef g;
    TempFile f("windows.tif");
    writeGeoTiff(f.path, w, h, data.data(), g, std::nullopt);
    auto src = TiffSource::open(f.path, 2);
    std::vector<double> vals(data.begin(), data.end());
    CHECK(readsBack(*src, vals));
    // Windows on the tile edges at 256 and 512.
    struct W { std::uint32_t c, r, w, h; };
    for (W win : {W{255, 255, 2, 2}, W{250, 0, 300, 300}, W{511, 255, 89, 45}, W{0, 0, 1, 1}, W{599, 299, 1, 1},
                  W{256, 256, 256, 44}, W{100, 200, 500, 100}}) {
        std::vector<double> out(std::size_t(win.w) * win.h);
        src->readWindow(win.c, win.r, win.w, win.h, out.data());
        bool ok = true;
        for (std::uint32_t y = 0; y < win.h; ++y)
            for (std::uint32_t x = 0; x < win.w; ++x)
                ok = ok && sameValue(out[std::size_t(y) * win.w + x], vals[std::size_t(win.r + y) * w + win.c + x]);
        CHECK(ok);
    }
    std::vector<double> out(4);
    CHECK(throwsSourceError([&] { src->readWindow(599, 0, 2, 1, out.data()); }));
    CHECK(throwsSourceError([&] { src->readWindow(0, 300, 1, 1, out.data()); }));
    CHECK(throwsSourceError([&] { src->readWindow(0xFFFFFFFFu, 0, 2, 1, out.data()); }));
}

TEST(threads_share_one_source) {
    const std::uint32_t w = 600, h = 300;
    const auto data = writerData(w, h);
    TempFile f("threads.tif");
    writeGeoTiff(f.path, w, h, data.data(), Georef{}, std::nullopt);
    auto src = TiffSource::open(f.path, 3);
    std::atomic<int> bad{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t)
        pool.emplace_back([&, t] {
            synth::Rng r(std::uint64_t(t) + 100);
            std::vector<double> out;
            for (int k = 0; k < 100; ++k) {
                const std::uint32_t c0 = std::uint32_t(r.next() % w), r0 = std::uint32_t(r.next() % h);
                const std::uint32_t ww = 1 + std::uint32_t(r.next() % std::min<std::uint32_t>(300, w - c0));
                const std::uint32_t hh = 1 + std::uint32_t(r.next() % std::min<std::uint32_t>(300, h - r0));
                out.resize(std::size_t(ww) * hh);
                src->readWindow(c0, r0, ww, hh, out.data());
                for (std::uint32_t y = 0; y < hh; ++y)
                    for (std::uint32_t x = 0; x < ww; ++x)
                        if (!sameValue(out[std::size_t(y) * ww + x], data[std::size_t(r0 + y) * w + c0 + x])) ++bad;
                if (k == 50 && t == 0) src->setCacheLimit(1);
            }
        });
    for (auto& th : pool) th.join();
    CHECK(bad == 0);
}

// ── Hand-built files: every sample type, codec, predictor, layout, order ─────

TEST(hand_built_matrix) {
    const SampleType types[] = {SampleType::U8,  SampleType::I16, SampleType::U16, SampleType::I32,
                                SampleType::U32, SampleType::F32, SampleType::F64};
    int files = 0, failed = 0;
    for (SampleType t : types)
        for (std::uint16_t comp : compressions())
            for (std::uint16_t pred : {std::uint16_t(1), std::uint16_t(2), std::uint16_t(3)}) {
                if (pred == 3 && sampleFormat(t) != 3) continue;
                for (bool tiled : {false, true})
                    for (bool be : {false, true})
                        for (bool big : {false, true}) {
                            Spec s;
                            s.type = t;
                            s.compression = comp;
                            s.predictor = pred;
                            s.tiled = tiled;
                            s.bigEndian = be;
                            s.bigTiff = big;
                            const auto vals = testValues(t, s.width, s.height, std::uint64_t(files));
                            TempFile f("matrix.tif");
                            f.write(buildFile(s, vals));
                            ++files;
                            bool ok = false;
                            try {
                                auto src = TiffSource::open(f.path);
                                const auto& in = src->info();
                                ok = in.width == s.width && in.height == s.height && in.sample == t &&
                                     src->layout().bigTiff == big && src->layout().bigEndian == be &&
                                     src->layout().tiled == tiled && src->layout().compression == comp &&
                                     src->layout().predictor == pred && readsBack(*src, vals);
                                ok = ok && in.georef.model == ModelType::Projected && in.georef.epsg &&
                                     *in.georef.epsg == 25830 && in.georef.originX == 440000 &&
                                     in.georef.originY == 4470000 && in.georef.spacingX == 0.5 &&
                                     in.georef.spacingY == 0.25 && in.noData && *in.noData == -9999;
                            } catch (const std::exception& e) {
                                std::printf("  %s: %s\n", s.name().c_str(), e.what());
                            }
                            if (!ok) {
                                ++failed;
                                if (failed < 10) std::printf("  failed: %s\n", s.name().c_str());
                            }
                        }
            }
    std::printf("  %d hand-built files\n", files);
    const int nc = int(compressions().size());
    CHECK(files == 7 * nc * 2 * 8 + 2 * nc * 8);
    CHECK(failed == 0);
}

#ifdef PYRE_HAVE_ZSTD
/// A block of one value compresses far past Deflate's 1032:1; ZSTD must still read it.
TEST(zstd_constant_block) {
    Spec s;
    s.type = SampleType::F32;
    s.compression = 50000;
    s.tiled = true;
    s.width = 256;
    s.height = 256;
    s.blockW = s.blockH = 256;   // one tile of 256 KB: thousands of times smaller in ZSTD
    std::vector<double> vals(std::size_t(s.width) * s.height, 612.5);
    TempFile f("zstd_const.tif");
    const auto bytes = buildFile(s, vals);
    // The tile alone: 256 KB in less than 96 bytes, which the guard for Deflate
    // (more than 2048:1 is damage) would have refused.
    const std::vector<std::uint8_t> tile(256 * 256 * 4, 0x44);
    CHECK(zstdCompress(tile).size() < 96);
    f.write(bytes);
    auto src = TiffSource::open(f.path);
    CHECK(src->layout().compression == 50000);
    CHECK(readsBack(*src, vals));
}
#else
TEST(zstd_refused_without_support) {
    // Hand-made: a file that says compression 50000 is refused with the reason.
    Spec s;
    s.compression = 50000;
    TempFile f("zstd_none.tif");
    f.write(buildFile(s, testValues(s.type, s.width, s.height, 1)));
    bool said = false;
    try {
        TiffSource::open(f.path);
    } catch (const SourceError& e) {
        said = std::string(e.what()).find("ZSTD") != std::string::npos;
    }
    CHECK(said);
}
#endif

TEST(lzw_long_data) {
    // Enough codes to grow to 12 bits and to fill the table several times.
    Spec s;
    s.type = SampleType::U8;
    s.compression = 5;
    s.width = 400;
    s.height = 300;
    s.rowsPerStrip = 300;
    std::vector<double> vals(std::size_t(s.width) * s.height);
    synth::Rng r(3);
    for (std::size_t i = 0; i < vals.size(); ++i) vals[i] = double((i % 7 == 0) ? r.next() % 256 : (i / 50) % 256);
    TempFile f("lzw.tif");
    f.write(buildFile(s, vals));
    auto src = TiffSource::open(f.path);
    CHECK(readsBack(*src, vals));
    // All 256 values repeated: long strings.
    for (std::size_t i = 0; i < vals.size(); ++i) vals[i] = double(i % 251 < 200 ? 17 : i % 256);
    f.write(buildFile(s, vals));
    CHECK(readsBack(*TiffSource::open(f.path), vals));
}

TEST(georef_variants) {
    Spec s;
    s.width = 8;
    s.height = 4;
    s.rowsPerStrip = 4;
    const auto vals = testValues(s.type, s.width, s.height, 1);
    TempFile f("georef.tif");

    // A tie point at raster (I, J) = (10, 20), PixelIsPoint, a geographic CRS
    // with a vertical CRS, the unit in GDAL_METADATA.
    {
        TIfd d = imageIfd(s, vals);
        d.tags.push_back(tDouble(33550, {0.25, 0.5, 0}));
        d.tags.push_back(tDouble(33922, {10, 20, 0, 100, 200, 0}));
        d.tags.push_back(tShort(34735, {1, 1, 0, 4, 1024, 0, 1, 2, 1025, 0, 1, 2, 2048, 0, 1, 4258, 4096, 0, 1, 5782}));
        d.tags.push_back(tAscii(42112, "<GDALMetadata>\n  <Item name=\"UNITTYPE\" sample=\"0\" role=\"unittype\">metre</Item>\n</GDALMetadata>"));
        d.tags.push_back(tAscii(42113, " -32767 "));
        f.write(assemble(false, false, {d}));
        auto src = TiffSource::open(f.path);
        const auto& g = src->info().georef;
        CHECK(g.model == ModelType::Geographic);
        CHECK(g.epsg && *g.epsg == 4258);
        CHECK(g.verticalEpsg && *g.verticalEpsg == 5782);
        CHECK(g.rasterType == RasterType::PixelIsPoint);
        CHECK(g.originX == 100 - 10 * 0.25);
        CHECK(g.originY == 200 + 20 * 0.5);
        CHECK(g.spacingX == 0.25 && g.spacingY == 0.5);
        CHECK(g.verticalUnit == "metre");
        CHECK(src->info().noData && *src->info().noData == -32767);
    }
    // User-defined CRS (32767) with citations; ModelTransformation.
    {
        TIfd d = imageIfd(s, vals);
        d.tags.push_back(tDouble(34264, {2, 0, 0, 500, 0, -3, 0, 900, 0, 0, 0, 0, 0, 0, 0, 1}));
        d.tags.push_back(tShort(34735, {1, 1, 0, 3, 1024, 0, 1, 1, 1026, 34737, 12, 0, 3072, 0, 1, 32767}));
        d.tags.push_back(tAscii(34737, "My own grid|"));
        f.write(assemble(true, false, {d}));
        auto src = TiffSource::open(f.path);
        const auto& g = src->info().georef;
        CHECK(g.model == ModelType::Projected);
        CHECK(!g.epsg);
        CHECK(g.wkt == "My own grid");
        CHECK(g.originX == 500 && g.originY == 900);
        CHECK(g.spacingX == 2 && g.spacingY == 3);
        CHECK(!src->info().noData);
    }
    // A rotated ModelTransformation is refused.
    {
        TIfd d = imageIfd(s, vals);
        d.tags.push_back(tDouble(34264, {2, 1, 0, 500, 0, -3, 0, 900, 0, 0, 0, 0, 0, 0, 0, 1}));
        f.write(assemble(false, false, {d}));
        CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    }
    // No georeference at all: the defaults.
    {
        f.write(assemble(false, true, {imageIfd(s, vals)}));
        auto src = TiffSource::open(f.path);
        const auto& g = src->info().georef;
        CHECK(g.model == ModelType::Unknown && !g.epsg && g.spacingX == 1 && g.originX == 0);
        CHECK(readsBack(*src, vals));
    }
}

TEST(overviews_and_masks_are_skipped) {
    Spec main;
    main.width = 20;
    main.height = 10;
    main.tiled = true;
    main.compression = 8;
    Spec ov = main;
    ov.width = 10;
    ov.height = 5;
    const auto vals = testValues(main.type, main.width, main.height, 5);
    const auto ovVals = testValues(ov.type, ov.width, ov.height, 6);
    TIfd overview = imageIfd(ov, ovVals);
    overview.tags.push_back(tLong(254, {1}));
    TIfd mask = imageIfd(ov, ovVals);
    mask.tags.push_back(tLong(254, {4}));
    TIfd full = imageIfd(main, vals);
    full.tags.push_back(tLong(254, {0}));
    TempFile f("ovr.tif");
    f.write(assemble(false, true, {overview, mask, full, overview}));
    auto src = TiffSource::open(f.path);
    CHECK(src->info().width == 20 && src->info().height == 10);
    CHECK(readsBack(*src, vals));
    // Only overviews: no image.
    f.write(assemble(false, false, {overview}));
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
}

TEST(sparse_tiles_read_as_nodata) {
    Spec s;
    s.tiled = true;
    s.compression = 8;
    auto vals = testValues(s.type, s.width, s.height, 9);
    TIfd d = imageIfd(s, vals, 1);   // block 1: columns 16..31 of rows 0..15
    d.tags.push_back(tAscii(42113, "-32767"));
    TempFile f("sparse.tif");
    f.write(assemble(false, false, {d}));
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 16; x < 32; ++x) vals[std::size_t(y) * s.width + x] = -32767;
    CHECK(readsBack(*TiffSource::open(f.path), vals));
}

TEST(unsupported_files_throw) {
    Spec s;
    s.width = 4;
    s.height = 4;
    s.rowsPerStrip = 4;
    const auto vals = testValues(s.type, s.width, s.height, 1);
    TempFile f("unsupported.tif");
    auto replace = [&](std::uint16_t tag, TTag nt) {
        TIfd d = imageIfd(s, vals);
        for (auto& t : d.tags)
            if (t.tag == tag) t = nt;
        return d;
    };
    f.write(assemble(false, false, {replace(277, tShort(277, {3}))}));
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    f.write(assemble(false, false, {replace(259, tShort(259, {7}))}));   // JPEG
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    f.write(assemble(false, false, {replace(258, tShort(258, {12}))}));
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    {
        Spec si = s;
        si.type = SampleType::I16;
        si.predictor = 3;
        TIfd d = imageIfd(si, testValues(si.type, 4, 4, 1));
        f.write(assemble(false, false, {d}));
        CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    }
    f.write({'I', 'I', 42, 0});
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    f.write({'P', 'K', 3, 4, 0, 0, 0, 0});
    CHECK(throwsSourceError([&] { TiffSource::open(f.path); }));
    CHECK(throwsSourceError([&] { TiffSource::open((tempDir() / "does-not-exist.tif").string()); }));
    CHECK(throwsSourceError([&] { openSource("terrain.asc"); }));
    // openSource dispatches .tif and .TIFF to the TIFF reader.
    TempFile g("upper.TIFF");
    g.write(buildFile(s, vals));
    CHECK(openSource(g.path)->info().width == 4);
}

// ── Damaged files ────────────────────────────────────────────────────────────

TEST(truncated_files_throw) {
    std::vector<std::vector<std::uint8_t>> files;
    {
        const std::uint32_t w = 300, h = 70;
        const auto data = writerData(w, h);
        TempFile f("trunc-src.tif");
        Georef g;
        g.model = ModelType::Geographic;
        g.epsg = 4326;
        g.verticalUnit = "metre";
        writeGeoTiff(f.path, w, h, data.data(), g, -32767.0);
        files.push_back(readAll(f.path));
    }
    std::vector<std::uint16_t> truncComps = {5, 8};
#ifdef PYRE_HAVE_ZSTD
    truncComps.push_back(50000);
#endif
    for (std::uint16_t comp : truncComps) {
        Spec s;
        s.compression = comp;
        s.predictor = 2;
        s.tiled = comp != 5;
        s.bigTiff = comp == 5;
        s.bigEndian = comp == 5;
        files.push_back(buildFile(s, testValues(s.type, s.width, s.height, comp)));
    }
    TempFile f("trunc.tif");
    int cases = 0, threw = 0;
    for (const auto& full : files) {
        const std::size_t step = std::max<std::size_t>(1, full.size() / 300);
        for (std::size_t len = 0; len < full.size(); len += (len < 64 ? 1 : step)) {
            f.write(std::vector<std::uint8_t>(full.begin(), full.begin() + std::ptrdiff_t(len)));
            ++cases;
            threw += openAndRead(f.path) == 0;
        }
    }
    std::printf("  %d truncated files, %d threw\n", cases, threw);
    CHECK(threw == cases);
}

TEST(damaged_files_do_not_crash) {
    std::vector<std::vector<std::uint8_t>> files;
    {
        const std::uint32_t w = 300, h = 70;
        const auto data = writerData(w, h);
        TempFile f("damage-src.tif");
        Georef g;
        g.model = ModelType::Geographic;
        g.epsg = 4326;
        writeGeoTiff(f.path, w, h, data.data(), g, -32767.0);
        files.push_back(readAll(f.path));
    }
    std::vector<std::uint16_t> damageComps = {1, 5, 8};
#ifdef PYRE_HAVE_ZSTD
    damageComps.push_back(50000);
#endif
    for (std::uint16_t comp : damageComps) {
        Spec s;
        s.type = comp == 1 ? SampleType::I16 : SampleType::F32;
        s.compression = comp;
        s.predictor = comp == 8 || comp == 50000 ? 3 : 2;
        s.tiled = comp != 5;
        s.bigTiff = comp == 5;
        s.bigEndian = comp == 1;
        files.push_back(buildFile(s, testValues(s.type, s.width, s.height, comp)));
    }
    TempFile f("damage.tif");
    synth::Rng r(2024);
    int cases = 0, read = 0;
    for (const auto& orig : files) {
        for (int k = 0; k < 400; ++k) {
            auto bytes = orig;
            const int mode = k % 4;
            const int n = 1 + int(r.next() % 8);
            for (int j = 0; j < n; ++j) {
                // Damage the header and the IFD more often than the blocks.
                const std::size_t region = (mode == 0) ? bytes.size() : std::min<std::size_t>(bytes.size(), 64);
                std::size_t pos = std::size_t(r.next() % region);
                if (mode >= 2) pos = bytes.size() - 1 - std::size_t(r.next() % std::min<std::size_t>(bytes.size(), 600));
                if (mode == 3)
                    bytes[pos] = std::uint8_t(r.next());
                else
                    bytes[pos] ^= std::uint8_t(1u << (r.next() % 8));
            }
            f.write(bytes);
            ++cases;
            read += openAndRead(f.path);
        }
    }
    std::printf("  %d damaged files: %d read, %d threw SourceError\n", cases, read, cases - read);
    CHECK(cases > 0);
}

// ── A real sheet (only when the data drive is mounted) ───────────────────────

TEST(real_mdt02_sheet) {
    const std::string path =
        "/Volumes/KELLERMAN/PublicData/mdt02/PUBLICACION_CNIG_DATOS_VARIOS_MDT02_WGS84/MDT02-WGS84-0117-1-COB2.tif";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::printf("  (skipped: the data drive is not mounted)\n");
        return;
    }
    auto src = openSource(path);
    const auto& in = src->info();
    CHECK(in.width == 9616);
    CHECK(in.height == 4989);
    CHECK(in.sample == SampleType::F32);
    CHECK(in.georef.model == ModelType::Geographic);
    CHECK(in.georef.epsg && *in.georef.epsg == 4326);
    CHECK(in.georef.rasterType == RasterType::PixelIsPoint);
    CHECK(std::fabs(in.georef.spacingX - 0.000018) < 1e-15);
    CHECK(std::fabs(in.georef.spacingY - 0.000018) < 1e-15);
    CHECK(std::fabs(in.georef.originX - -1.190594) < 1e-6);
    CHECK(std::fabs(in.georef.originY - 43.003208) < 1e-6);
    CHECK(in.noData && *in.noData == -32767);
    CHECK(in.georef.verticalUnit == "metre");
    auto* tiff = dynamic_cast<const TiffSource*>(src.get());
    REQUIRE(tiff);
    CHECK(tiff->layout().bigTiff);
    CHECK(tiff->layout().tiled && tiff->layout().blockWidth == 512);

    // The middle of the sheet: heights in metres (Navarre, near the Pyrenees).
    const std::uint32_t w = 1000, h = 1000;
    std::vector<double> out(std::size_t(w) * h);
    src->readWindow(4300, 2000, w, h, out.data());
    std::size_t valid = 0, plausible = 0;
    double lo = 1e9, hi = -1e9;
    for (double v : out) {
        if (in.isNoData(v)) continue;
        ++valid;
        plausible += v > -50 && v < 3500;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    std::printf("  window: %zu valid, heights %.2f .. %.2f m\n", valid, lo, hi);
    CHECK(valid > 0);
    CHECK(plausible == valid);
    // The last row and column read too.
    std::vector<double> edge(9616);
    src->readWindow(0, 4988, 9616, 1, edge.data());
    std::vector<double> col(4989);
    src->readWindow(9615, 0, 1, 4989, col.data());
}

CHECK_MAIN
