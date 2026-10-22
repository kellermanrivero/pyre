#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  crc32c.hpp — CRC-32C (Castagnoli), the checksum of the file format
// ═════════════════════════════════════════════════════════════════════════════
//
// The CPU's instruction where there is one (ARMv8 CRC, x86 SSE 4.2), else a
// table, eight bytes at a time. All give the same value. Check value:
// crc32c("123456789") = 0xE3069283.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define PYRE_CRC32C_ARM 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define PYRE_CRC32C_SSE 1
#endif

namespace pyre {

namespace detail {
inline constexpr std::uint32_t kCrcPoly = 0x82F63B78u;   // reflected 0x1EDC6F41

constexpr std::array<std::array<std::uint32_t, 256>, 8> makeCrcTables() {
    std::array<std::array<std::uint32_t, 256>, 8> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1u) ? kCrcPoly : 0u);
        t[0][i] = c;
    }
    for (std::size_t s = 1; s < 8; ++s)
        for (std::uint32_t i = 0; i < 256; ++i) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    return t;
}
inline constexpr auto kCrcTables = makeCrcTables();

/// The table version, for the tests (it must agree with the CPU's).
inline std::uint32_t crc32cTable(const std::uint8_t* p, std::size_t n, std::uint32_t crc) {
    const auto& t = kCrcTables;
    while (n >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);   // little-endian (see container.hpp)
        v ^= crc;
        crc = t[7][v & 0xFF] ^ t[6][(v >> 8) & 0xFF] ^ t[5][(v >> 16) & 0xFF] ^ t[4][(v >> 24) & 0xFF] ^
              t[3][(v >> 32) & 0xFF] ^ t[2][(v >> 40) & 0xFF] ^ t[1][(v >> 48) & 0xFF] ^ t[0][v >> 56];
        p += 8;
        n -= 8;
    }
    while (n--) crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 0xFFu];
    return crc;
}
}  // namespace detail

/// CRC-32C of n bytes, continuing from `previous` (0 to start).
inline std::uint32_t crc32c(const void* data, std::size_t n, std::uint32_t previous = 0) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = ~previous;
#if defined(PYRE_CRC32C_ARM)
    while (n >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        crc = __crc32cd(crc, v);
        p += 8;
        n -= 8;
    }
    while (n--) crc = __crc32cb(crc, *p++);
#elif defined(PYRE_CRC32C_SSE)
    std::uint64_t c = crc;
    while (n >= 8) {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
        p += 8;
        n -= 8;
    }
    crc = std::uint32_t(c);
    while (n--) crc = _mm_crc32_u8(crc, *p++);
#else
    crc = detail::crc32cTable(p, n, crc);
#endif
    return ~crc;
}

}  // namespace pyre
