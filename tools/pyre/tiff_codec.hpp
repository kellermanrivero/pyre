#pragma once

// ═════════════════════════════════════════════════════════════════════════════
//  tiff_codec.hpp — the TIFF block codecs (internal to tiff.cpp and the writer)
// ═════════════════════════════════════════════════════════════════════════════
//
// Each decoder fills exactly `outSize` bytes, or throws SourceError.

#include <cstddef>
#include <cstdint>

namespace pyre::tool::tiffcodec {

/// Inflates one zlib stream (TIFF compression 8 and 32946). The stream must
/// give exactly outSize bytes.
void inflateExact(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, std::size_t outSize);

/// True if this build reads ZSTD (TIFF compression 50000; PYRE_WITH_ZSTD).
bool zstdAvailable();

/// The decoded size a ZSTD frame says it has, if it says (no frame or no size: 0).
std::uint64_t zstdFrameSize(const std::uint8_t* in, std::size_t inSize);

/// Decompresses ZSTD frames (TIFF compression 50000). They must give exactly
/// outSize bytes. Throws SourceError, also when this build has no ZSTD.
void zstdDecompressExact(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, std::size_t outSize);

/// Decodes TIFF LZW (compression 5: codes MSB first, 9 to 12 bits, early
/// change). The output must be exactly outSize bytes; data after the last
/// needed code is ignored.
void lzwDecodeExact(const std::uint8_t* in, std::size_t inSize, std::uint8_t* out, std::size_t outSize);

/// Reverses the byte order of `count` samples of `bytes` bytes (2, 4 or 8).
void byteSwap(std::uint8_t* data, std::size_t count, unsigned bytes);

/// Undoes predictor 2 on `rows` rows of `rowSamples` native-endian samples.
void undoHorizontal(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes);

/// Undoes predictor 3 (libtiff's floating-point predictor) on `rows` rows of
/// `rowSamples` samples of `bytes` bytes. The result is native-endian.
void undoFloatingPoint(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes);

/// Applies predictor 3 to native-endian samples (the writer's side).
void applyFloatingPoint(std::uint8_t* data, std::size_t rowSamples, std::size_t rows, unsigned bytes);

}  // namespace pyre::tool::tiffcodec
