#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// On MSVC, x64 builds enable SSE2 by default; /arch:AVX2 is opt-in
// and must be passed by the build system to enable the AVX2 path.
// GCC/Clang honor -mavx2 and define __AVX2__.

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC)) \
    || (defined(_M_IX86) && defined(_M_IX86_FP) && (_M_IX86_FP >= 2))
#define SOFTCAM_HAS_SSE2 1
#include <emmintrin.h>
#endif

#if defined(__SSSE3__) || (defined(_M_X64) && !defined(_M_ARM64EC))
#define SOFTCAM_HAS_SSSE3 1
#include <tmmintrin.h>
#endif

#if defined(__AVX2__)
#define SOFTCAM_HAS_AVX2 1
#include <immintrin.h>
#endif


namespace softcam {


// ---------------------------------------------------------------------------
// RGBA (R,G,B,A byte order) -> BGRA (B,G,R,A byte order)
//
// Used at the producer boundary because Dart's ui.Image.toByteData with
// rawRgba emits RGBA byte order, but the v3 shared-memory wire is BGRA32
// so the receiver-side FillBuffer does NOT need to touch the channel
// order (only the row order, which transferToDIB already handles).
//
// Shuffle mask per 16-byte SSE2 register (low-to-high): {2,1,0,3, 6,5,4,7,
// 10,9,8,11, 14,13,12,15} -- swap R<->B in each pixel, leave A in place.
// ---------------------------------------------------------------------------
#if defined(SOFTCAM_HAS_SSSE3)
inline void convert_rgba_to_bgra(const std::uint8_t* src,
                                 std::uint8_t*       dst,
                                 std::size_t         pixel_count)
{
    // SSSE3 pshufb mask that swaps R<->B per pixel and keeps G, A in place.
    // Per pixel (4 bytes) we want:
    //   out[ 0]=src[ 2] out[ 1]=src[ 1] out[ 2]=src[ 0] out[ 3]=src[ 3]
    //   out[ 4]=src[ 6] out[ 5]=src[ 5] out[ 6]=src[ 4] out[ 7]=src[ 7]
    //   out[ 8]=src[10] out[ 9]=src[ 9] out[10]=src[ 8] out[11]=src[11]
    //   out[12]=src[14] out[13]=src[13] out[14]=src[12] out[15]=src[15]
    //
    // _mm_set_epi8 args are listed highest-byte first; the last arg is
    // byte 0 of the mask, so the call below reads low-to-high as
    // 2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15.
    const __m128i swap_rb = _mm_set_epi8(
        15, 12, 13, 14,
        11,  8,  9, 10,
         7,  4,  5,  6,
         3,  0,  1,  2);
    std::size_t i = 0;
    for (; i + 4 <= pixel_count; i += 4)
    {
        __m128i in = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i * 4));
        __m128i out = _mm_shuffle_epi8(in, swap_rb);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 4), out);
    }
    for (; i < pixel_count; ++i)
    {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}
#else
inline void convert_rgba_to_bgra(const std::uint8_t* src,
                                 std::uint8_t*       dst,
                                 std::size_t         pixel_count)
{
    for (std::size_t i = 0; i < pixel_count; ++i)
    {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}
#endif


// All converters in this file consume the v3 wire format:
//   src     : top-down BGRA32, pixel_count * 4 bytes
//   width, height : frame dimensions (>= 2 for YUV 4:2:0 planar)
//
// Colorimetry: BT.601 full-range (JPEG/PC style). Y,U,V in [16,235]/[16,240]
// would be BT.601 limited-range; we deliberately pick full-range because
// virtual cameras feeding desktop apps (OBS, Zoom, Chrome getUserMedia)
// expect JPEG-style color.
//
// Coeffs (matches libjpeg-turbo / FFmpeg jpeg-yuv tables):
//   Y  =  0.29900 R + 0.58700 G + 0.11400 B
//   Cb = -0.16874 R - 0.33126 G + 0.50000 B + 128
//   Cr =  0.50000 R - 0.41869 G - 0.08131 B + 128
// All clipped to [0,255].
//
// Fixed-point scale: Q10 (divide by 1024). The more precise Q16 has
// coefficient 0.587 * 65536 = 38471 which overflows int16 (max 32767)
// and triggers MSVC C4309 truncation; Q10 keeps every coefficient below
// 1024 and fits in int16 with at most 1 LSB accuracy loss.


// ---------------------------------------------------------------------------
// Pack 4 BGRA pixels (16 input bytes) into 4 Y values using SSE2.
//
// Output: 4 Y bytes in a __m128i low quad, zero-extended.
//
// Q10 fixed-point coefficients (max 601) fit comfortably in int16; the
// more precise Q16 values would require 38470 which overflows int16
// (max 32767) and triggers MSVC C4309 truncation.
// ---------------------------------------------------------------------------
#if defined(SOFTCAM_HAS_SSE2)
inline __m128i bgra_to_y_sse2(__m128i bgra_quad)
{
    // Unpack BGRA bytes into 16-bit lanes.
    // After unpacklo_epi8 (low 8 bytes interleaved with zero):
    //   [B0, G0, R0, A0, B1, G1, R1, A1]  (16-bit lanes)
    // After unpackhi_epi8 (high 8 bytes interleaved with zero):
    //   [B2, G2, R2, A2, B3, G3, R3, A3]
    const __m128i zero = _mm_setzero_si128();
    __m128i b16 = _mm_unpacklo_epi8(bgra_quad, zero);
    __m128i r16 = _mm_unpackhi_epi8(bgra_quad, zero);

    // For each pixel: Y = (117*B + 601*G + 306*R + 512) >> 10.
    //
    // Use _mm_madd_epi16 to do two-coefficient multiply-add per lane:
    //   lane 0 = B*117 + G*601
    //   lane 1 = R*306 + A*0
    //   lane 2 = (pixel 1) B*117 + G*601
    //   lane 3 = (pixel 1) R*306 + A*0
    //
    // Then a horizontal-add of lane 0+1 gives Y for pixel 0; lane 2+3
    // gives Y for pixel 1. Same for the high half.
    //
    // coef in low-to-high order: [117, 601, 306, 0, 117, 601, 306, 0]
    const __m128i coef_y = _mm_set_epi16(0, 306, 601, 117, 0, 306, 601, 117);
    __m128i y_lo = _mm_madd_epi16(b16, coef_y);
    __m128i y_hi = _mm_madd_epi16(r16, coef_y);

    // Horizontal sum: lane0+lane1, lane2+lane3 (no SSE3 hadd -- use
    // shuffle+add which is SSE2).
    __m128i y_lo_sum = _mm_add_epi32(y_lo, _mm_shuffle_epi32(y_lo, _MM_SHUFFLE(2,3,0,1)));
    __m128i y_hi_sum = _mm_add_epi32(y_hi, _mm_shuffle_epi32(y_hi, _MM_SHUFFLE(2,3,0,1)));
    // y_lo_sum = [Y0, Y0, Y1, Y1]; y_hi_sum = [Y2, Y2, Y3, Y3].
    // Combine via _mm_shuffle_ps so we get one Y per pixel:
    //   out[0]=y_lo_sum[0]=Y0, out[1]=y_lo_sum[2]=Y1,
    //   out[2]=y_hi_sum[0]=Y2, out[3]=y_hi_sum[2]=Y3.
    __m128i y_packed = _mm_castps_si128(_mm_shuffle_ps(
        _mm_castsi128_ps(y_lo_sum),
        _mm_castsi128_ps(y_hi_sum),
        _MM_SHUFFLE(2, 0, 2, 0)));

    // Q10 scale: round to nearest, shift right by 10.
    const __m128i round = _mm_set1_epi32(512);
    y_packed = _mm_srli_epi32(_mm_add_epi32(y_packed, round), 10);

    // Pack to 8-bit (saturating) and zero-extend to 16-bit so callers
    // can use _mm_extract_epi16 with the same low-half layout as the
    // scalar path.
    __m128i y8 = _mm_packus_epi32(y_packed, y_packed);
    __m128i y16 = _mm_unpacklo_epi8(y8, zero);
    return y16; // Y0 Y1 Y2 Y3 in low 4 lanes
}
#endif


// Scalar BT.601 RGB->YUV, full-range, with rounding to match SSE2 path.
// Q10 fixed-point (max abs coefficient 601 fits in int16).
inline std::uint8_t rgb_to_y(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    int y = (306 * r + 601 * g + 117 * b + 512) >> 10;
    if (y < 0) y = 0; else if (y > 255) y = 255;
    return static_cast<std::uint8_t>(y);
}

inline std::uint8_t rgb_to_cb(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Cb = -0.16874 R - 0.33126 G + 0.50000 B + 128
    //    = (-173 R - 339 G + 512 B) / 1024 + 128
    int v = (-173 * r - 339 * g + 512 * b + 512 + 128 * 1024) >> 10;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return static_cast<std::uint8_t>(v);
}

inline std::uint8_t rgb_to_cr(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    // Cr =  0.50000 R - 0.41869 G - 0.08131 B + 128
    //    = (512 R - 274 G - 53 B) / 1024 + 128
    int v = (512 * r - 274 * g - 53 * b + 512 + 128 * 1024) >> 10;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return static_cast<std::uint8_t>(v);
}


// ---------------------------------------------------------------------------
// BGRA32 -> YUY2 (4:2:2 packed, [Y0 U Y1 V] per 2 input pixels)
// ---------------------------------------------------------------------------
//
// src_bgra: width * height * 4 bytes, top-down
// dst_yuy2: width * height * 2 bytes, top-down
// All chroma is averaged between adjacent pixel pairs.

inline void convert_bgra_to_yuy2(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst,
                    std::size_t            pixel_count)
{
    const std::uint8_t* sp = src;
    std::uint8_t* dp = dst;
    std::size_t i = 0;
#if defined(SOFTCAM_HAS_SSE2)
    // Process 4 BGRA pixels -> 8 YUY2 bytes (two [Y0 U Y1 V] quads).
    // We use 2 SSE registers worth of input and 1 register worth of output
    // for each pair. The 4 Y values come from bgra_to_y_sse2 applied to two
    // separate loads (the low 4 pixels and the high 4 pixels of an 8-pixel
    // group). For chroma we average per pair; the simple approach here is
    // scalar for clarity -- the SIMD savings on the U/V path are minor
    // compared to Y, which dominates SSE bandwidth.
    for (; i + 2 <= pixel_count; i += 2)
    {
        // Load 2 BGRA pixels (8 bytes) into low quad.
        __m128i q = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(sp));
        __m128i y16 = bgra_to_y_sse2(q);
        int y0 = _mm_extract_epi16(y16, 0);
        int y1 = _mm_extract_epi16(y16, 1);

        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        std::uint8_t u = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;
        std::uint8_t v = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;

        dp[0] = static_cast<std::uint8_t>(y0);
        dp[1] = u;
        dp[2] = static_cast<std::uint8_t>(y1);
        dp[3] = v;
        sp += 8;
        dp += 4;
    }
#endif
    for (; i < pixel_count; i += 2)
    {
        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        dp[0] = rgb_to_y(r0, g0, b0);
        dp[1] = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;
        dp[2] = rgb_to_y(r1, g1, b1);
        dp[3] = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;
        sp += 8;
        dp += 4;
    }
}


// ---------------------------------------------------------------------------
// BGRA32 -> UYVY (4:2:2 packed, [U Y0 V Y1] per 2 input pixels)
// ---------------------------------------------------------------------------

inline void convert_bgra_to_uyvy(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst,
                    std::size_t            pixel_count)
{
    const std::uint8_t* sp = src;
    std::uint8_t* dp = dst;
    std::size_t i = 0;
#if defined(SOFTCAM_HAS_SSE2)
    for (; i + 2 <= pixel_count; i += 2)
    {
        __m128i q = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(sp));
        __m128i y16 = bgra_to_y_sse2(q);
        int y0 = _mm_extract_epi16(y16, 0);
        int y1 = _mm_extract_epi16(y16, 1);

        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        std::uint8_t u = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;
        std::uint8_t v = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;

        dp[0] = u;
        dp[1] = static_cast<std::uint8_t>(y0);
        dp[2] = v;
        dp[3] = static_cast<std::uint8_t>(y1);
        sp += 8;
        dp += 4;
    }
#endif
    for (; i < pixel_count; i += 2)
    {
        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        dp[0] = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;
        dp[1] = rgb_to_y(r0, g0, b0);
        dp[2] = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;
        dp[3] = rgb_to_y(r1, g1, b1);
        sp += 8;
        dp += 4;
    }
}


// ---------------------------------------------------------------------------
// BGRA32 -> YVYU (4:2:2 packed, [Y0 V Y1 U] per 2 input pixels)
// ---------------------------------------------------------------------------

inline void convert_bgra_to_yvyu(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst,
                    std::size_t            pixel_count)
{
    const std::uint8_t* sp = src;
    std::uint8_t* dp = dst;
    std::size_t i = 0;
#if defined(SOFTCAM_HAS_SSE2)
    for (; i + 2 <= pixel_count; i += 2)
    {
        __m128i q = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(sp));
        __m128i y16 = bgra_to_y_sse2(q);
        int y0 = _mm_extract_epi16(y16, 0);
        int y1 = _mm_extract_epi16(y16, 1);

        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        std::uint8_t v = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;
        std::uint8_t u = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;

        dp[0] = static_cast<std::uint8_t>(y0);
        dp[1] = v;
        dp[2] = static_cast<std::uint8_t>(y1);
        dp[3] = u;
        sp += 8;
        dp += 4;
    }
#endif
    for (; i < pixel_count; i += 2)
    {
        std::uint8_t b0 = sp[0], g0 = sp[1], r0 = sp[2];
        std::uint8_t b1 = sp[4], g1 = sp[5], r1 = sp[6];
        dp[0] = rgb_to_y(r0, g0, b0);
        dp[1] = (rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1) + 1) >> 1;
        dp[2] = rgb_to_y(r1, g1, b1);
        dp[3] = (rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1) + 1) >> 1;
        sp += 8;
        dp += 4;
    }
}


// ---------------------------------------------------------------------------
// 4:2:0 planar converters.
//
// Caller passes three output planes (Y, U, V) plus their strides. The Y
// plane uses the input width * stride_bytes; chroma planes are (w/2) wide
// and use the caller-provided chroma stride.
//
// src_bgra: width * height * 4 bytes (top-down BGRA)
// dst_y, dst_u, dst_v: planar destinations
// y_stride, u_stride, v_stride: per-row byte strides for each plane

inline void bgra_quad_to_yuv_scalar(const std::uint8_t* q,
                                    std::uint8_t& y0, std::uint8_t& y1,
                                    std::uint8_t& y2, std::uint8_t& y3,
                                    std::uint8_t& cb, std::uint8_t& cr)
{
    std::uint8_t b0 = q[ 0], g0 = q[ 1], r0 = q[ 2];
    std::uint8_t b1 = q[ 4], g1 = q[ 5], r1 = q[ 6];
    std::uint8_t b2 = q[ 8], g2 = q[ 9], r2 = q[10];
    std::uint8_t b3 = q[12], g3 = q[13], r3 = q[14];

    y0 = rgb_to_y(r0, g0, b0);
    y1 = rgb_to_y(r1, g1, b1);
    y2 = rgb_to_y(r2, g2, b2);
    y3 = rgb_to_y(r3, g3, b3);

    // Chroma average across the 2x2 block.
    int cb_acc = rgb_to_cb(r0, g0, b0) + rgb_to_cb(r1, g1, b1)
               + rgb_to_cb(r2, g2, b2) + rgb_to_cb(r3, g3, b3);
    int cr_acc = rgb_to_cr(r0, g0, b0) + rgb_to_cr(r1, g1, b1)
               + rgb_to_cr(r2, g2, b2) + rgb_to_cr(r3, g3, b3);
    cb = static_cast<std::uint8_t>((cb_acc + 2) >> 2);
    cr = static_cast<std::uint8_t>((cr_acc + 2) >> 2);
}


inline void convert_bgra_to_iyuv(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst_y, std::uint32_t y_stride,
                    std::uint8_t*          dst_u, std::uint32_t u_stride,
                    std::uint8_t*          dst_v, std::uint32_t v_stride,
                    std::uint32_t          width, std::uint32_t height)
{
    for (std::uint32_t y = 0; y < height; y += 2)
    {
        const std::uint8_t* row0 = src + (std::size_t)y * width * 4;
        const std::uint8_t* row1 = src + (std::size_t)(y + 1) * width * 4;
        std::uint8_t* yptr0 = dst_y + (std::size_t)y * y_stride;
        std::uint8_t* yptr1 = dst_y + (std::size_t)(y + 1) * y_stride;
        std::uint8_t* uptr  = dst_u + (std::size_t)(y / 2) * u_stride;
        std::uint8_t* vptr  = dst_v + (std::size_t)(y / 2) * v_stride;
        for (std::uint32_t x = 0; x < width; x += 2)
        {
            std::uint8_t y0, y1, y2, y3, cb, cr;
            bgra_quad_to_yuv_scalar(row0 + x * 4, y0, y1, y2, y3, cb, cr);
            yptr0[x]     = y0;
            yptr0[x + 1] = y1;
            yptr1[x]     = y2;
            yptr1[x + 1] = y3;
            uptr[x / 2]  = cb;
            vptr[x / 2]  = cr;
        }
    }
}


// IYUV == I420. The two MEDIASUBTYPEs share the same byte layout.
inline void convert_bgra_to_i420(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst_y, std::uint32_t y_stride,
                    std::uint8_t*          dst_u, std::uint32_t u_stride,
                    std::uint8_t*          dst_v, std::uint32_t v_stride,
                    std::uint32_t          width, std::uint32_t height)
{
    convert_bgra_to_iyuv(src, dst_y, y_stride, dst_u, u_stride, dst_v, v_stride,
                         width, height);
}


// YV12 == I420 with U and V planes swapped.
inline void convert_bgra_to_yv12(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst_y, std::uint32_t y_stride,
                    std::uint8_t*          dst_u, std::uint32_t u_stride,
                    std::uint8_t*          dst_v, std::uint32_t v_stride,
                    std::uint32_t          width, std::uint32_t height)
{
    convert_bgra_to_iyuv(src, dst_y, y_stride, dst_v, v_stride, dst_u, u_stride,
                         width, height);
}


// NV12: Y plane full-res + UV plane interleaved at half res.
inline void convert_bgra_to_nv12(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst_y, std::uint32_t y_stride,
                    std::uint8_t*          dst_uv, std::uint32_t uv_stride,
                    std::uint32_t          width, std::uint32_t height)
{
    for (std::uint32_t y = 0; y < height; y += 2)
    {
        const std::uint8_t* row0 = src + (std::size_t)y * width * 4;
        const std::uint8_t* row1 = src + (std::size_t)(y + 1) * width * 4;
        std::uint8_t* yptr0 = dst_y + (std::size_t)y * y_stride;
        std::uint8_t* yptr1 = dst_y + (std::size_t)(y + 1) * y_stride;
        std::uint8_t* uvptr = dst_uv + (std::size_t)(y / 2) * uv_stride;
        for (std::uint32_t x = 0; x < width; x += 2)
        {
            std::uint8_t y0, y1, y2, y3, cb, cr;
            bgra_quad_to_yuv_scalar(row0 + x * 4, y0, y1, y2, y3, cb, cr);
            yptr0[x]     = y0;
            yptr0[x + 1] = y1;
            yptr1[x]     = y2;
            yptr1[x + 1] = y3;
            std::uint32_t ux = x;
            uvptr[ux]     = cb;
            uvptr[ux + 1] = cr;
        }
    }
}


// ---------------------------------------------------------------------------
// BGRA32 -> Y800 (monochrome, luma only)
//
// Standard "luma" weights: Y = 0.299*R + 0.587*G + 0.114*B, full range.
// Identical math to RGB->Y in 4:2:2 / 4:2:0 -- we just discard chroma.
// ---------------------------------------------------------------------------

inline void convert_bgra_to_y800(
                    const std::uint8_t*    src,
                    std::uint8_t*          dst,
                    std::size_t            pixel_count)
{
    const std::uint8_t* sp = src;
    std::uint8_t* dp = dst;
    std::size_t i = 0;
#if defined(SOFTCAM_HAS_SSE2)
    for (; i + 4 <= pixel_count; i += 4)
    {
        __m128i q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sp));
        __m128i y16 = bgra_to_y_sse2(q);
        // Pack four 16-bit Y values back into 4 8-bit bytes.
        std::uint8_t y0 = static_cast<std::uint8_t>(_mm_extract_epi16(y16, 0));
        std::uint8_t y1 = static_cast<std::uint8_t>(_mm_extract_epi16(y16, 1));
        std::uint8_t y2 = static_cast<std::uint8_t>(_mm_extract_epi16(y16, 2));
        std::uint8_t y3 = static_cast<std::uint8_t>(_mm_extract_epi16(y16, 3));
        dp[0] = y0; dp[1] = y1; dp[2] = y2; dp[3] = y3;
        sp += 16;
        dp += 4;
    }
#endif
    for (; i < pixel_count; ++i)
    {
        std::uint8_t b = sp[0], g = sp[1], r = sp[2];
        dp[0] = rgb_to_y(r, g, b);
        sp += 4;
        dp += 1;
    }
}


} //namespace softcam