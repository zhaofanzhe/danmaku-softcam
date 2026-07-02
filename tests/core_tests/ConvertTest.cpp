#include <softcamcore/SimdConvert.h>
#include <gtest/gtest.h>

#include <vector>
#include <cstdint>
#include <cstdlib>


namespace ConvertTest {
namespace sc = softcam;


// Reference BT.601 full-range RGB->YUV implementation. Used as ground
// truth for SIMD converter tests. Q10 fixed-point so coefficients fit
// in int16; matches the production SimdConvert.h math exactly.
static std::uint8_t ref_y(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    int y = (306 * r + 601 * g + 117 * b + 512) >> 10;
    if (y < 0) y = 0; else if (y > 255) y = 255;
    return static_cast<std::uint8_t>(y);
}
static std::uint8_t ref_cb(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    int v = (-173 * r - 339 * g + 512 * b + 512 + 128 * 1024) >> 10;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return static_cast<std::uint8_t>(v);
}
static std::uint8_t ref_cr(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    int v = (512 * r - 274 * g - 53 * b + 512 + 128 * 1024) >> 10;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return static_cast<std::uint8_t>(v);
}


static std::vector<std::uint8_t> random_rgba(std::size_t n, std::uint32_t seed = 0xC0FFEE)
{
    std::vector<std::uint8_t> out(n * 4);
    std::uint32_t s = seed;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        s = s * 1103515245u + 12345u;
        out[i] = static_cast<std::uint8_t>((s >> 16) & 0xff);
    }
    return out;
}


// Tolerance: integer BT.601 math can have a 1-LSB rounding difference
// between SSE2 and scalar paths, so allow +/-1.
static void expect_near(std::uint8_t actual, std::uint8_t expected, int tol = 1,
                        const char* tag = nullptr)
{
    int diff = (int)actual - (int)expected;
    if (diff < 0) diff = -diff;
    EXPECT_LE(diff, tol) << tag << " actual=" << (int)actual
                          << " expected=" << (int)expected;
}


TEST(RgbaToBgra, SwapsRAndBKeepsGAndA) {
    auto src = random_rgba(64);
    std::vector<std::uint8_t> dst(64 * 4);

    sc::convert_rgba_to_bgra(src.data(), dst.data(), 64);

    for (std::size_t i = 0; i < 64; ++i)
    {
        EXPECT_EQ(dst[i * 4 + 0], src[i * 4 + 2]); // B <-> R
        EXPECT_EQ(dst[i * 4 + 1], src[i * 4 + 1]); // G unchanged
        EXPECT_EQ(dst[i * 4 + 2], src[i * 4 + 0]); // R <-> B
        EXPECT_EQ(dst[i * 4 + 3], src[i * 4 + 3]); // A unchanged
    }
}


TEST(BgraToY800, MatchesScalarReference) {
    auto src = random_rgba(256);
    std::vector<std::uint8_t> dst(256);
    sc::convert_bgra_to_y800(src.data(), dst.data(), 256);

    for (std::size_t i = 0; i < 256; ++i)
    {
        std::uint8_t r = src[i * 4 + 2];
        std::uint8_t g = src[i * 4 + 1];
        std::uint8_t b = src[i * 4 + 0];
        expect_near(dst[i], ref_y(r, g, b), 1, "y800");
    }
}


TEST(BgraToYUY2, LumaMatchesReferenceAndChromaIsAverage) {
    auto src = random_rgba(64); // 32 pixel pairs
    std::vector<std::uint8_t> dst(64 * 2); // w*h*2 bytes
    sc::convert_bgra_to_yuy2(src.data(), dst.data(), 64);

    for (std::size_t i = 0; i < 64; i += 2)
    {
        std::uint8_t y0_a = ref_y(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]);
        std::uint8_t y1_a = ref_y(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]);
        std::uint8_t cb_a = (ref_cb(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cb(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;
        std::uint8_t cr_a = (ref_cr(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cr(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;

        expect_near(dst[i * 2 + 0], y0_a, 1, "y0");
        expect_near(dst[i * 2 + 1], cb_a, 1, "u");
        expect_near(dst[i * 2 + 2], y1_a, 1, "y1");
        expect_near(dst[i * 2 + 3], cr_a, 1, "v");
    }
}


TEST(BgraToIYUV, PlanarLumaAndChromaBlockAverage) {
    const std::uint32_t w = 16, h = 16;
    auto src = random_rgba(w * h);
    std::vector<std::uint8_t> dst(w * h * 3 / 2);
    std::uint8_t* y_plane = dst.data();
    std::uint8_t* u_plane = y_plane + w * h;
    std::uint8_t* v_plane = u_plane + (w / 2) * (h / 2);

    sc::convert_bgra_to_i420(src.data(), y_plane, w, u_plane, w / 2, v_plane, w / 2, w, h);

    // Validate every Y, every U/V (block-averaged over 2x2).
    for (std::uint32_t y = 0; y < h; ++y)
    {
        for (std::uint32_t x = 0; x < w; ++x)
        {
            std::uint8_t r = src[(y * w + x) * 4 + 2];
            std::uint8_t g = src[(y * w + x) * 4 + 1];
            std::uint8_t b = src[(y * w + x) * 4 + 0];
            expect_near(y_plane[y * w + x], ref_y(r, g, b), 1, "y");
        }
    }
    for (std::uint32_t y = 0; y < h; y += 2)
    {
        for (std::uint32_t x = 0; x < w; x += 2)
        {
            auto avg = [&](auto getter) {
                int s = 0;
                for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    auto p = &src[((y + dy) * w + (x + dx)) * 4];
                    s += getter(p[2], p[1], p[0]);
                }
                return static_cast<std::uint8_t>((s + 2) >> 2);
            };
            expect_near(u_plane[(y / 2) * (w / 2) + (x / 2)],
                        avg(ref_cb), 1, "u");
            expect_near(v_plane[(y / 2) * (w / 2) + (x / 2)],
                        avg(ref_cr), 1, "v");
        }
    }
}


TEST(BgraToNV12, UVInterleavedAtHalfRes) {
    const std::uint32_t w = 16, h = 16;
    auto src = random_rgba(w * h);
    std::vector<std::uint8_t> dst(w * h * 3 / 2);
    std::uint8_t* y_plane = dst.data();
    std::uint8_t* uv_plane = y_plane + w * h;

    sc::convert_bgra_to_nv12(src.data(), y_plane, w, uv_plane, w, w, h);

    for (std::uint32_t y = 0; y < h; y += 2)
    {
        for (std::uint32_t x = 0; x < w; x += 2)
        {
            auto avg = [&](auto getter) {
                int s = 0;
                for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    auto p = &src[((y + dy) * w + (x + dx)) * 4];
                    s += getter(p[2], p[1], p[0]);
                }
                return static_cast<std::uint8_t>((s + 2) >> 2);
            };
            expect_near(uv_plane[(y / 2) * w + x + 0], avg(ref_cb), 1, "u");
            expect_near(uv_plane[(y / 2) * w + x + 1], avg(ref_cr), 1, "v");
        }
    }
}


TEST(BgraToUYVY, ChromaAtPositions0And2) {
    auto src = random_rgba(64);
    std::vector<std::uint8_t> dst(64 * 2);
    sc::convert_bgra_to_uyvy(src.data(), dst.data(), 64);

    for (std::size_t i = 0; i < 64; i += 2)
    {
        std::uint8_t y0_a = ref_y(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]);
        std::uint8_t y1_a = ref_y(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]);
        std::uint8_t cb_a = (ref_cb(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cb(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;
        std::uint8_t cr_a = (ref_cr(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cr(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;

        expect_near(dst[i * 2 + 0], cb_a, 1, "u");
        expect_near(dst[i * 2 + 1], y0_a, 1, "y0");
        expect_near(dst[i * 2 + 2], cr_a, 1, "v");
        expect_near(dst[i * 2 + 3], y1_a, 1, "y1");
    }
}


TEST(BgraToYVYU, ChromaIsVFirstThenU) {
    auto src = random_rgba(64);
    std::vector<std::uint8_t> dst(64 * 2);
    sc::convert_bgra_to_yvyu(src.data(), dst.data(), 64);

    for (std::size_t i = 0; i < 64; i += 2)
    {
        std::uint8_t y0_a = ref_y(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]);
        std::uint8_t y1_a = ref_y(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]);
        std::uint8_t cb_a = (ref_cb(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cb(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;
        std::uint8_t cr_a = (ref_cr(src[i * 4 + 2], src[i * 4 + 1], src[i * 4 + 0]) +
                            ref_cr(src[(i + 1) * 4 + 2], src[(i + 1) * 4 + 1], src[(i + 1) * 4 + 0]) + 1) >> 1;

        expect_near(dst[i * 2 + 0], y0_a, 1, "y0");
        expect_near(dst[i * 2 + 1], cr_a, 1, "v");
        expect_near(dst[i * 2 + 2], y1_a, 1, "y1");
        expect_near(dst[i * 2 + 3], cb_a, 1, "u");
    }
}


} //namespace ConvertTest