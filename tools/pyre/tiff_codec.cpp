// The TIFF block codecs: Deflate (zlib), LZW and the predictors.

#include "tiff_codec.hpp"

#include <zlib.h>

#include <bit>
#include <climits>
#include <cstring>
#include <vector>

#include "source.hpp"

namespace pyre::tool::tiffcodec {

namespace {

// One zlib stream for each thread, reset for each block (no allocation).
struct Inflater {
    z_stream s{};
    bool ok = false;
    Inflater() { ok = inflateInit(&s) == Z_OK; }
    ~Inflater() {
        if (ok) inflateEnd(&s);
    }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
};

template <class T>
void horizontalAcc(std::uint8_t* data, std::size_t rowSamples, std::size_t rows) {
    for (std::size_t r = 0; r < rows; ++r) {
        std::uint8_t* p = data + r * rowSamples * sizeof(T);
        T prev;
        std::memcpy(&prev, p, sizeof(T));
        for (std::size_t i = 1; i < rowSamples; ++i) {
            T v;
            std::memcpy(&v, p + i * sizeof(T), sizeof(T));
            v = T(v + prev);
            std::memcpy(p + i * sizeof(T), &v, sizeof(T));
            prev = v;
        }
    }
}

std::vector<std::uint8_t>& rowScratch(std::size_t n) {
    thread_local std::vector<std::uint8_t> tmp;
    if (tmp.size() < n) tmp.resize(n);
    return tmp;
}

}  // namespace

void inflateExact(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, std::size_t outSize) {
    thread_local Inflater inf;
    if (!inf.ok) throw SourceError("zlib: cannot start the inflater");
    if (inSize > UINT_MAX || outSize > UINT_MAX) throw SourceError("deflate: block too large");
    z_stream& s = inf.s;
    if (inflateReset(&s) != Z_OK) throw SourceError("zlib: cannot reset the inflater");
    s.next_in = const_cast<Bytef*>(in);
    s.avail_in = static_cast<uInt>(inSize);
    s.next_out = out;
    s.avail_out = static_cast<uInt>(outSize);
    const int r = inflate(&s, Z_FINISH);
    if (r == Z_STREAM_END) {
        if (s.avail_out != 0) throw SourceError("deflate: the block is too short");
        return;
    }
    if (r == Z_BUF_ERROR || r == Z_OK) {
        if (s.avail_out == 0) throw SourceError("deflate: the block is too long");
        throw SourceError("deflate: the block is truncated");
    }
    throw SourceError(std::string("deflate: bad data (") + (s.msg ? s.msg : "zlib error") + ")");
}

void lzwDecodeExact(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, std::size_t outSize) {
    constexpr unsigned kClear = 256, kEoi = 257, kFirst = 258, kMaxCodes = 4096;
    constexpr std::uint16_t kNone = 0xFFFF;
    if (inSize >= 2 && in[0] == 0 && (in[1] & 1))
        throw SourceError("LZW: old-style (LSB first) LZW is not supported");

    struct Entry {
        std::uint16_t prefix;
        std::uint16_t len;
        std::uint8_t first;
        std::uint8_t last;
    };
    thread_local std::vector<Entry> tableStore(kMaxCodes);
    Entry* table = tableStore.data();
    for (unsigned c = 0; c < 256; ++c) table[c] = {kNone, 1, std::uint8_t(c), std::uint8_t(c)};

    std::uint8_t* op = out;
    std::uint8_t* const end = out + outSize;
    std::uint64_t acc = 0;
    int nbits = 0;
    std::size_t ip = 0;
    unsigned codeLen = 9, nextCode = kFirst;
    unsigned oldCode = kNone;

    auto getCode = [&](unsigned w, unsigned& code) -> bool {
        while (nbits < int(w)) {
            if (ip >= inSize) return false;
            acc = (acc << 8) | in[ip++];
            nbits += 8;
        }
        nbits -= int(w);
        code = unsigned(acc >> nbits) & ((1u << w) - 1);
        return true;
    };
    auto emit = [&](unsigned code) {
        const unsigned len = table[code].len;
        if (std::size_t(end - op) < len) throw SourceError("LZW: the block is too long");
        std::uint8_t* p = op + len;
        unsigned c = code;
        while (c != kNone) {
            *--p = table[c].last;
            c = table[c].prefix;
        }
        op += len;
    };

    while (op < end) {
        unsigned code;
        if (!getCode(codeLen, code)) break;
        if (code == kEoi) break;
        if (code == kClear) {
            nextCode = kFirst;
            codeLen = 9;
            if (!getCode(codeLen, code) || code == kEoi) break;
            if (code >= 256) throw SourceError("LZW: bad code after a clear code");
            *op++ = std::uint8_t(code);
            oldCode = code;
            continue;
        }
        if (oldCode == kNone) {
            // No clear code first: allowed only for a literal.
            if (code >= 256) throw SourceError("LZW: bad first code");
            *op++ = std::uint8_t(code);
            oldCode = code;
            continue;
        }
        std::uint8_t firstByte;
        if (code < nextCode) {
            if (code == kClear || code == kEoi) throw SourceError("LZW: bad code");
            emit(code);
            firstByte = table[code].first;
        } else if (code == nextCode && nextCode < kMaxCodes) {
            // The KwKwK case: the string of the old code and its first byte.
            emit(oldCode);
            if (op >= end) throw SourceError("LZW: the block is too long");
            firstByte = table[oldCode].first;
            *op++ = firstByte;
        } else {
            throw SourceError("LZW: code out of the table");
        }
        if (nextCode < kMaxCodes) {
            const unsigned len = table[oldCode].len + 1u;
            table[nextCode] = {std::uint16_t(oldCode), std::uint16_t(len), table[oldCode].first, firstByte};
            ++nextCode;
            if (nextCode == (1u << codeLen) - 1 && codeLen < 12) ++codeLen;
        }
        oldCode = code;
    }
    if (op != end) throw SourceError("LZW: the block is too short");
}

void byteSwap(std::uint8_t* data, std::size_t count, unsigned bytes) {
    switch (bytes) {
        case 1: return;
        case 2:
            for (std::size_t i = 0; i < count; ++i) {
                std::uint16_t v;
                std::memcpy(&v, data + 2 * i, 2);
                v = std::uint16_t((v >> 8) | (v << 8));
                std::memcpy(data + 2 * i, &v, 2);
            }
            return;
        case 4:
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t v;
                std::memcpy(&v, data + 4 * i, 4);
                v = __builtin_bswap32(v);
                std::memcpy(data + 4 * i, &v, 4);
            }
            return;
        case 8:
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t v;
                std::memcpy(&v, data + 8 * i, 8);
                v = __builtin_bswap64(v);
                std::memcpy(data + 8 * i, &v, 8);
            }
            return;
        default: throw SourceError("bad sample size");
    }
}

void undoHorizontal(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes) {
    if (rowSamples == 0) return;
    switch (bytes) {
        case 1: horizontalAcc<std::uint8_t>(data, rowSamples, rows); return;
        case 2: horizontalAcc<std::uint16_t>(data, rowSamples, rows); return;
        case 4: horizontalAcc<std::uint32_t>(data, rowSamples, rows); return;
        case 8: horizontalAcc<std::uint64_t>(data, rowSamples, rows); return;
        default: throw SourceError("predictor 2: bad sample size");
    }
}

// libtiff's fpAcc: (1) undo the byte differencing along the whole row; (2) the
// row holds byte planes, the most significant byte first; put them back into
// native-endian samples.
void undoFloatingPoint(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes) {
    if (bytes != 2 && bytes != 4 && bytes != 8) throw SourceError("predictor 3: bad sample size");
    const std::size_t rowBytes = rowSamples * bytes;
    if (rowBytes == 0) return;
    std::uint8_t* tmp = rowScratch(rowBytes).data();
    constexpr bool little = std::endian::native == std::endian::little;
    for (std::size_t r = 0; r < rows; ++r) {
        std::uint8_t* p = data + r * rowBytes;
        std::uint8_t s = p[0];
        tmp[0] = s;
        for (std::size_t i = 1; i < rowBytes; ++i) {
            s = std::uint8_t(s + p[i]);
            tmp[i] = s;
        }
        if (bytes == 4) {
            const std::uint8_t* b0 = tmp;  // most significant
            const std::uint8_t* b1 = tmp + rowSamples;
            const std::uint8_t* b2 = tmp + 2 * rowSamples;
            const std::uint8_t* b3 = tmp + 3 * rowSamples;
            for (std::size_t i = 0; i < rowSamples; ++i) {
                // The planes give the value most significant byte first; the
                // store is native.
                const std::uint32_t v = (std::uint32_t(b0[i]) << 24) | (std::uint32_t(b1[i]) << 16) |
                                        (std::uint32_t(b2[i]) << 8) | std::uint32_t(b3[i]);
                std::memcpy(p + 4 * i, &v, 4);
            }
        } else {
            for (std::size_t i = 0; i < rowSamples; ++i)
                for (unsigned b = 0; b < bytes; ++b) {
                    // Plane b holds byte b of the big-endian value.
                    const unsigned dst = little ? bytes - 1 - b : b;
                    p[i * bytes + dst] = tmp[b * rowSamples + i];
                }
        }
    }
}

void applyFloatingPoint(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes) {
    if (bytes != 2 && bytes != 4 && bytes != 8) throw SourceError("predictor 3: bad sample size");
    const std::size_t rowBytes = rowSamples * bytes;
    if (rowBytes == 0) return;
    std::uint8_t* tmp = rowScratch(rowBytes).data();
    constexpr bool little = std::endian::native == std::endian::little;
    for (std::size_t r = 0; r < rows; ++r) {
        std::uint8_t* p = data + r * rowBytes;
        for (std::size_t i = 0; i < rowSamples; ++i)
            for (unsigned b = 0; b < bytes; ++b) {
                const unsigned src = little ? bytes - 1 - b : b;
                tmp[b * rowSamples + i] = p[i * bytes + src];
            }
        p[0] = tmp[0];
        for (std::size_t i = 1; i < rowBytes; ++i) p[i] = std::uint8_t(tmp[i] - tmp[i - 1]);
    }
}

}  // namespace pyre::tool::tiffcodec
