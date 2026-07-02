#include <softcamcore/MjpegEncoder.h>
#include <gtest/gtest.h>

#include <vector>
#include <cstdint>


namespace MjpegEncoderTest {
namespace sc = softcam;


// Magic bytes for a JFIF / baseline JPEG: FF D8 FF E0 (APP0 segment).
// The encoder always emits a JFIF wrapper.
static bool starts_with_jfif(const std::uint8_t* buf, std::size_t len)
{
    return len >= 4 && buf[0] == 0xFF && buf[1] == 0xD8 &&
                       buf[2] == 0xFF && buf[3] == 0xE0;
}


TEST(MjpegEncoder, EncodesValidJpegHeader) {
    sc::MjpegEncoder enc;
    const int W = 32;
    const int H = 24;

    std::vector<std::uint8_t> bgra(W * H * 4, 0x80);
    // Vary the pixels so the encoder has actual data to compress.
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            std::uint8_t* p = &bgra[(y * W + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 8);
            p[1] = static_cast<std::uint8_t>(y * 8);
            p[2] = static_cast<std::uint8_t>((x + y) * 4);
            p[3] = 0xFF;
        }
    }

    std::vector<std::uint8_t> jpeg(W * H * 2, 0);
    std::size_t written = enc.encode(bgra.data(), W, H, jpeg.data(), jpeg.size());

    EXPECT_GT(written, 0u);
    EXPECT_LE(written, jpeg.size());
    EXPECT_TRUE(starts_with_jfif(jpeg.data(), written));
}


TEST(MjpegEncoder, EncodesSmallerThanBGRA) {
    // At quality 80, a low-entropy gradient frame should compress to a
    // fraction of its BGRA32 size.
    sc::MjpegEncoder enc;
    const int W = 256;
    const int H = 256;

    std::vector<std::uint8_t> bgra(W * H * 4, 0x40);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            std::uint8_t* p = &bgra[(y * W + x) * 4];
            p[0] = static_cast<std::uint8_t>(x & 0xFF);
            p[1] = static_cast<std::uint8_t>(y & 0xFF);
            p[2] = 0x80;
            p[3] = 0xFF;
        }
    }

    std::vector<std::uint8_t> jpeg(W * H, 0); // 1 byte/pixel budget
    std::size_t written = enc.encode(bgra.data(), W, H, jpeg.data(), jpeg.size());

    EXPECT_GT(written, 0u);
    // 256x256x1 = 65536 byte cap; gradient should fit comfortably.
    EXPECT_LE(written, jpeg.size());
}


TEST(MjpegEncoder, ReturnsZeroOnBufferTooSmall) {
    sc::MjpegEncoder enc;
    const int W = 64;
    const int H = 64;

    std::vector<std::uint8_t> bgra(W * H * 4, 0xAA);
    std::vector<std::uint8_t> jpeg(16, 0); // way too small

    std::size_t written = enc.encode(bgra.data(), W, H, jpeg.data(), jpeg.size());
    // The WIC stream initialization will fail because the JPEG can't fit
    // into 16 bytes -- we expect the encoder to bail with 0.
    EXPECT_EQ(written, 0u);
}


} //namespace MjpegEncoderTest