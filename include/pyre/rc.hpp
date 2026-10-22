#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  rc.hpp — LZMA's binary range coder, with 15-bit probabilities
// ═════════════════════════════════════════════════════════════════════════════
//
// The reference (heightmap.hpp, format 3) uses this coder as it is. LZMA uses
// 11-bit probabilities: with a shift of 5, the probability of an event does not
// go below 31/2048, and a sure "zero" costs at least 0.022 bits. A tile of sea
// is a million zeros, so that floor is 2.7 KB a tile. With 15 bits the floor is
// 31/32768, about 0.0014 bits a node. The range still fits:
// (range >> 15) · p with range ≥ 2^24 and p < 2^15.
//
// Two framings of a stream:
//   Lzma     the stream as LZMA writes it: a first byte that is always 0, then a
//            flush of 5 bytes. The reference's framing.
//   Trimmed  the first byte is not written; the flush writes the shortest value
//            in the final interval; trailing zero bytes are cut. The decoder
//            reads zeros past the end, so a cut stream decodes the same.
//            About 4 bytes less a stream.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pyre {

inline constexpr int kProbBits = 15;
inline constexpr std::uint16_t kProbInit = 1u << (kProbBits - 1);
inline constexpr int kShift = 5;
inline constexpr std::uint32_t kTop = 1u << 24;

enum class Framing : std::uint8_t { Lzma = 0, Trimmed = 1 };

class Encoder {
public:
    Encoder(std::vector<std::uint8_t>& out, Framing framing) : out_(out), framing_(framing) {}

    /// Codes bit `b` with the adaptive probability `p` (the probability of a 0).
    void bit(std::uint16_t& p, bool b) {
        const std::uint32_t bound = (range_ >> kProbBits) * p;
        if (!b) {
            range_ = bound;
            p = std::uint16_t(p + (((1u << kProbBits) - p) >> kShift));
        } else {
            low_ += bound;
            range_ -= bound;
            p = std::uint16_t(p - (p >> kShift));
        }
        while (range_ < kTop) { range_ <<= 8; shiftLow(); }
    }

    /// The `n` low bits of `v`, the highest first, each with probability 1/2.
    void raw(std::uint32_t v, int n) {
        for (int k = n - 1; k >= 0; --k) {
            range_ >>= 1;
            if ((v >> k) & 1u) low_ += range_;
            while (range_ < kTop) { range_ <<= 8; shiftLow(); }
        }
    }

    void finish() {
        if (framing_ == Framing::Trimmed) {
            // The value in [low, low + range) with its low 24 bits zero: it
            // exists because range ≥ 2^24.
            constexpr std::uint64_t mask = kTop - 1;
            low_ = (low_ + mask) & ~mask;
        }
        for (int k = 0; k < 5; ++k) shiftLow();
        if (framing_ == Framing::Trimmed)
            while (!out_.empty() && out_.back() == 0) out_.pop_back();
    }

private:
    void shiftLow() {
        if (std::uint32_t(low_) < 0xFF000000u || (low_ >> 32) != 0) {
            const std::uint8_t carry = std::uint8_t(low_ >> 32);
            std::uint8_t t = cache_;
            do {
                // The first byte of an LZMA stream is always 0: Trimmed drops it.
                if (!first_ || framing_ == Framing::Lzma) out_.push_back(std::uint8_t(t + carry));
                first_ = false;
                t = 0xFF;
            } while (--pending_ != 0);
            cache_ = std::uint8_t(std::uint32_t(low_) >> 24);
        }
        ++pending_;
        low_ = (low_ & 0x00FFFFFFu) << 8;
    }

    std::vector<std::uint8_t>& out_;
    std::uint64_t low_ = 0;
    std::uint32_t range_ = 0xFFFFFFFFu;
    std::uint8_t cache_ = 0;
    std::uint64_t pending_ = 1;
    Framing framing_;
    bool first_ = true;
};

/// The decoder never reads outside its bytes: past the end it reads zeros.
class Decoder {
public:
    Decoder(const std::uint8_t* data, std::size_t n, Framing framing) : d_(data), n_(n) {
        const int first = framing == Framing::Lzma ? 5 : 4;
        for (int k = 0; k < first; ++k) code_ = (code_ << 8) | byte();
    }

    bool bit(std::uint16_t& p) {
        const std::uint32_t bound = (range_ >> kProbBits) * p;
        bool b;
        if (code_ < bound) {
            range_ = bound;
            p = std::uint16_t(p + (((1u << kProbBits) - p) >> kShift));
            b = false;
        } else {
            code_ -= bound;
            range_ -= bound;
            p = std::uint16_t(p - (p >> kShift));
            b = true;
        }
        while (range_ < kTop) { range_ <<= 8; code_ = (code_ << 8) | byte(); }
        return b;
    }

    std::uint32_t raw(int n) {
        std::uint32_t v = 0;
        for (int k = 0; k < n; ++k) {
            range_ >>= 1;
            std::uint32_t b = 0;
            if (code_ >= range_) { code_ -= range_; b = 1; }
            v = (v << 1) | b;
            while (range_ < kTop) { range_ <<= 8; code_ = (code_ << 8) | byte(); }
        }
        return v;
    }

    /// Bytes read past the end of the data.
    std::size_t pastEnd() const { return past_; }

private:
    std::uint32_t byte() {
        if (i_ < n_) return d_[i_++];
        ++past_;
        return 0;
    }
    const std::uint8_t* d_;
    std::size_t n_, i_ = 0, past_ = 0;
    std::uint32_t code_ = 0, range_ = 0xFFFFFFFFu;
};

}  // namespace pyre
