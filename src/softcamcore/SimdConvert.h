#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// On MSVC, x64 builds enable SSE2 and SSSE3 by default; /arch:AVX2 is opt-in
// and must be passed by the build system to enable the AVX2 path. GCC/Clang
// honor -mavx2 and define __AVX2__.

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__SSSE3__) || (defined(_M_X64) && !defined(_M_ARM64EC)) \
    || (defined(_M_IX86) && defined(_M_IX86_FP) && (_M_IX86_FP >= 2))
#define SOFTCAM_HAS_SSSE3 1
#include <tmmintrin.h>
#endif

#if defined(__AVX2__)
#define SOFTCAM_HAS_AVX2 1
#include <immintrin.h>
#endif


namespace softcam {


// Convert tightly-packed RGBA (R,G,B,A bytes per pixel) to BGR byte order
// (B,G,R bytes per pixel) -- the layout DirectShow consumers expect when
// reading RGB24 / BI_RGB DIB samples.
//
// Source layout: [R0 G0 B0 A0 | R1 G1 B1 A1 | ...] (4 bytes per pixel)
// Dest layout:   [B0 G0 R0      | B1 G1 R1      | ...] (3 bytes per pixel)
//
// Both buffers must be at least `pixel_count * 4` and `pixel_count * 3` bytes
// respectively. The source is assumed opaque (alpha == 0xFF everywhere),
// so premultiplied RGBA equals straight RGBA -- no per-pixel alpha division.
//
// Implementation:
//   - AVX2 path processes 8 pixels per iteration via two 128-bit SSSE3-style
//     shuffles packed into a 256-bit register (the shuffle operates per-lane).
//   - SSSE3 path processes 4 pixels per iteration.
//   - Scalar tail handles remainder pixels.
//
// Each SSE/AVX store writes to a stack buffer first, then memcpy's the exact
// 12/24-byte slice to the destination, avoiding any overrun past the end of
// the destination buffer.
inline void convert_rgba_to_bgr(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst,
                    std::size_t            pixel_count)
{
#if defined(SOFTCAM_HAS_AVX2)
    // AVX2 mask: applied per 128-bit lane. Each lane processes 4 RGBA pixels
    // (16 bytes) and produces 12 bytes BGR + 4 padding bytes.
    // -1 in a shuffle mask byte means "output zero" -- the SSSE3 pshufb
    // semantic. We use plain -1 to avoid MSVC C4310 "cast truncates" warnings
    // on (char)0xFF.
    //
    // _mm256_set_epi8 args are listed highest-byte first; the last arg is
    // byte 0.  Mask layout per 128-bit lane (output bytes 0..15):
    //   output[0]=src[2]   output[1]=src[1]   output[2]=src[0]   -- pixel 0 BGR
    //   output[3]=src[6]   output[4]=src[5]   output[5]=src[4]   -- pixel 1 BGR
    //   output[6]=src[10]  output[7]=src[9]   output[8]=src[8]   -- pixel 2 BGR
    //   output[9]=src[14]  output[10]=src[13] output[11]=src[12] -- pixel 3 BGR
    //   output[12..15] = 0
    const __m256i mask = _mm256_set_epi8(
        // High 128-bit lane (bytes 31..16 of the 256-bit register).
        -1, -1, -1, -1,
        12, 13, 14,
         8,  9, 10,
         4,  5,  6,
         0,  1,  2,
        // Low 128-bit lane (bytes 15..0).
        -1, -1, -1, -1,
        12, 13, 14,
         8,  9, 10,
         4,  5,  6,
         0,  1,  2);

    std::size_t i = 0;
    for (; i + 8 <= pixel_count; i += 8)
    {
        __m256i in = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(src + i * 4));
        __m256i out = _mm256_shuffle_epi8(in, mask);
        alignas(32) std::uint8_t tmp[32];
        _mm256_store_si256(reinterpret_cast<__m256i*>(tmp), out);
        std::memcpy(dst + i * 3, tmp, 24);
    }
    for (; i < pixel_count; i++)
    {
        dst[i * 3 + 0] = src[i * 4 + 2];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 0];
    }
#elif defined(SOFTCAM_HAS_SSSE3)
    // Same mask layout as the AVX2 path, but only one 128-bit lane.
    const __m128i mask = _mm_set_epi8(
        -1, -1, -1, -1,
        12, 13, 14,
         8,  9, 10,
         4,  5,  6,
         0,  1,  2);

    std::size_t i = 0;
    for (; i + 4 <= pixel_count; i += 4)
    {
        __m128i in = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(src + i * 4));
        __m128i out = _mm_shuffle_epi8(in, mask);
        alignas(16) std::uint8_t tmp[16];
        _mm_store_si128(reinterpret_cast<__m128i*>(tmp), out);
        std::memcpy(dst + i * 3, tmp, 12);
    }
    for (; i < pixel_count; i++)
    {
        dst[i * 3 + 0] = src[i * 4 + 2];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 0];
    }
#else
    for (std::size_t i = 0; i < pixel_count; i++)
    {
        dst[i * 3 + 0] = src[i * 4 + 2];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 0];
    }
#endif
}


} //namespace softcam