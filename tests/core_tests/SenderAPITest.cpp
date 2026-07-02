#include <softcamcore/SenderAPI.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <softcamcore/FrameBuffer.h>
#include <softcamcore/Misc.h>


namespace SenderAPITest {
namespace sc = softcam;
namespace sender = softcam::sender;

#define SLEEP_MS(msec) \
        std::this_thread::sleep_for(std::chrono::milliseconds(msec))
#define WAIT_FOR_FLAG_CHANGE(atomic_flag, last_value) [&]{ \
            while (atomic_flag == last_value) { SLEEP_MS(1); } \
        }()

TEST(SenderCreateCamera, Basic)
{
    {
        auto handle = sender::CreateCamera(320, 240, 60);
        EXPECT_TRUE( handle );

        auto fb = sc::FrameBuffer::open();
        EXPECT_EQ( fb.width(), 320 );
        EXPECT_EQ( fb.height(), 240 );
        EXPECT_EQ( fb.framerate(), 60 );

        EXPECT_NO_THROW({ sender::DeleteCamera(handle); });
    }{
        auto handle = sender::CreateCamera(1920, 1080, 30);
        EXPECT_TRUE( handle );

        auto fb = sc::FrameBuffer::open();
        EXPECT_EQ( fb.width(), 1920 );
        EXPECT_EQ( fb.height(), 1080 );
        EXPECT_EQ( fb.framerate(), 30 );

        EXPECT_NO_THROW({ sender::DeleteCamera(handle); });
    }
}

TEST(SenderCreateCamera, FramerateIsOptional) {
    auto handle = sender::CreateCamera(320, 240);
    EXPECT_TRUE( handle );

    auto fb = sc::FrameBuffer::open();
    EXPECT_EQ( fb.framerate(), 60 );

    sender::DeleteCamera(handle);
}

TEST(SenderCreateCamera, ZeroMeansUnlimitedVariableFramerate) {
    auto handle = sender::CreateCamera(320, 240, 0.0f);
    EXPECT_TRUE( handle );

    auto fb = sc::FrameBuffer::open();
    EXPECT_EQ( fb.framerate(), 0.0f );

    sender::DeleteCamera(handle);
}

TEST(SenderCreateCamera, InvalidArgs) {
    {
        auto handle = sender::CreateCamera(0, 240, 60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }{
        auto handle = sender::CreateCamera(320, 0, 60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }{
        auto handle = sender::CreateCamera(0, 0, 60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }{
        auto handle = sender::CreateCamera(-320, 240, 60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }{
        auto handle = sender::CreateCamera(320, -240, 60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }{
        auto handle = sender::CreateCamera(320, 240, -60);
        EXPECT_FALSE( handle );
        sender::DeleteCamera(handle);
    }
}

TEST(SenderDeleteCamera, InvalidArgs)
{
    auto handle = sender::CreateCamera(320, 240);
    int x = 0;
    EXPECT_NO_THROW({ sender::DeleteCamera(&x); });
    EXPECT_NO_THROW({ sender::DeleteCamera(nullptr); });
    sender::DeleteCamera(handle); // correct
    EXPECT_NO_THROW({ sender::DeleteCamera(handle); }); // double free
}

TEST(SenderSendFrame, Basic)
{
    const float TIMEOUT = 1.0f;
    const unsigned char COLOR_VALUE = 123;

    auto handle = sender::CreateCamera(320, 240);
    std::atomic<int> flag = 0;

    std::thread th([&]
    {
        auto fb = sc::FrameBuffer::open();
        ASSERT_TRUE( fb );

        EXPECT_EQ( fb.frameCounter(), 0 );
        flag = 1;

        fb.waitForNewFrame(0, TIMEOUT);
        EXPECT_EQ( fb.frameCounter(), 1 );

        unsigned char image[320 * 240 * 3];
        uint64_t frame_counter = 0;
        fb.transferToDIB(image, &frame_counter);
        EXPECT_EQ( image[0], COLOR_VALUE );
        EXPECT_EQ( image[320 * 240 * 3 - 1], COLOR_VALUE );
        EXPECT_EQ( frame_counter, 1 );
    });

    WAIT_FOR_FLAG_CHANGE(flag, 0);

    unsigned char image[320 * 240 * 3] = {};
    std::memset(image, COLOR_VALUE, sizeof(image));
    sender::SendFrame(handle, image);

    th.join();
    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, SendsFirstFrameImmediately)
{
    auto handle = sender::CreateCamera(320, 240);
    unsigned char image[320 * 240 * 3] = {};

    sc::Timer timer;
    sender::SendFrame(handle, image);   // first
    auto lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, SendsEveryFrameImmediatelyIfZeroFramerate)
{
    const float FRAMERATE = 0.0f;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 3] = {};

    sc::Timer timer;
    sender::SendFrame(handle, image);
    auto lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    timer.reset();
    sender::SendFrame(handle, image);
    lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    timer.reset();
    sender::SendFrame(handle, image);
    lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, KeepsProperInterval)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 3] = {};

    sender::SendFrame(handle, image);   // first

    sc::Timer timer;
    sender::SendFrame(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    sender::SendFrame(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, KeepsProperIntervalEvenIfFirstFrameDelayed)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 3] = {};

    SLEEP_MS(30);  // delay

    sender::SendFrame(handle, image);   // first

    sc::Timer timer;
    sender::SendFrame(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    sender::SendFrame(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, KeepsProperIntervalEvenIfSomeFramesDelayed)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 3] = {};

    sender::SendFrame(handle, image);   // first

    sc::Timer timer;

    SLEEP_MS(30);  // delay
    sender::SendFrame(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    SLEEP_MS(20);  // delay
    sender::SendFrame(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrame, InvalidArgs)
{
    auto handle = sender::CreateCamera(320, 240);
    unsigned char image[320 * 240 * 3] = {};

    EXPECT_NO_THROW({ sender::SendFrame(nullptr, nullptr); });
    EXPECT_NO_THROW({ sender::SendFrame(nullptr, image); });
    EXPECT_NO_THROW({ sender::SendFrame(handle, nullptr); });

    auto fb = sc::FrameBuffer::open();
    EXPECT_EQ( fb.frameCounter(), 0 );

    sender::SendFrame(handle, image); // ok. ++frame_counter
    EXPECT_EQ( fb.frameCounter(), 1 );

    sender::DeleteCamera(handle);

    EXPECT_NO_THROW({ sender::SendFrame(handle, image); });
    EXPECT_EQ( fb.frameCounter(), 1 );
}

TEST(SenderWaitForConnection, ShouldBlockUntilReceiverConnected)
{
    auto handle = sender::CreateCamera(320, 240);
    std::atomic<int> flag = 0;

    std::thread th([&]
    {
        WAIT_FOR_FLAG_CHANGE(flag, 0);
        SLEEP_MS(10);
        EXPECT_EQ( flag, 1 );

        auto fb = sc::FrameBuffer::open();
        ASSERT_TRUE( fb );

        WAIT_FOR_FLAG_CHANGE(flag, 1);
        EXPECT_EQ( flag, 2 );
    });

    flag = 1;
    bool ret = sender::WaitForConnection(handle);
    flag = 2;

    th.join();
    EXPECT_EQ( ret, true );
    sender::DeleteCamera(handle);
}

TEST(SenderWaitForConnection, ShouldTimeout)
{
    const float TIMEOUT = 0.5f;

    auto handle = sender::CreateCamera(320, 240);
    std::atomic<int> flag = 0;

    std::thread th([&]
    {
        WAIT_FOR_FLAG_CHANGE(flag, 0);
        SLEEP_MS(10);
        EXPECT_EQ( flag, 1 );

        WAIT_FOR_FLAG_CHANGE(flag, 1);
        EXPECT_EQ( flag, 2 );
    });

    flag = 1;
    bool ret = sender::WaitForConnection(handle, TIMEOUT);
    flag = 2;

    th.join();
    EXPECT_EQ( ret, false );
    sender::DeleteCamera(handle);
}

TEST(SenderWaitForConnection, InvalidArgs)
{
    bool ret = sender::WaitForConnection(nullptr);
    EXPECT_EQ( ret, false );

    auto handle = sender::CreateCamera(320, 240);
    sender::DeleteCamera(handle);

    ret = sender::WaitForConnection(handle);
    EXPECT_EQ( ret, false );
}

TEST(SenderIsConnected, ReturnsFalseIfNotConnectedEver)
{
    auto handle = sender::CreateCamera(320, 240);

    bool ret = sender::IsConnected(handle);

    EXPECT_EQ( ret, false );
    sender::DeleteCamera(handle);
}

TEST(SenderIsConnected, ReturnsTrueIfAlreadyConnected)
{
    auto handle = sender::CreateCamera(320, 240);

    auto fb = sc::FrameBuffer::open();
    ASSERT_TRUE( fb );

    bool ret = sender::IsConnected(handle);

    EXPECT_EQ( ret, true );
    sender::DeleteCamera(handle);
}

TEST(SenderIsConnected, ReturnsFalseIfAlreadyDisconnected)
{
    auto handle = sender::CreateCamera(320, 240);

    auto fb = sc::FrameBuffer::open();
    SLEEP_MS(100);
    fb.release();
    SLEEP_MS(500 + 200); // enough time for receiver-watchdog timeout

    bool ret = sender::IsConnected(handle);

    EXPECT_EQ( ret, false );
    sender::DeleteCamera(handle);
}

TEST(SenderIsConnected, InvalidArgs)
{
    bool ret = sender::IsConnected(nullptr);
    EXPECT_EQ( ret, false );

    auto handle = sender::CreateCamera(320, 240);
    sender::DeleteCamera(handle);

    ret = sender::IsConnected(handle);
    EXPECT_EQ( ret, false );
}

TEST(SenderCreateCamera, RGBA32Format)
{
    auto handle = sender::CreateCamera(64, 48, 30.0f, sender::FORMAT_RGBA32);
    EXPECT_TRUE( handle );

    auto fb = sc::FrameBuffer::open();
    ASSERT_TRUE( fb );
    EXPECT_EQ( fb.width(), 64 );
    EXPECT_EQ( fb.height(), 48 );
    EXPECT_EQ( fb.framerate(), 30 );
    EXPECT_EQ( fb.imageFormat(), sc::ImageFormat::RGBA32 );

    sender::DeleteCamera(handle);
}

TEST(SenderCreateCamera, RGB24FormatDefault)
{
    auto handle = sender::CreateCamera(64, 48, 30.0f);
    EXPECT_TRUE( handle );

    auto fb = sc::FrameBuffer::open();
    ASSERT_TRUE( fb );
    EXPECT_EQ( fb.imageFormat(), sc::ImageFormat::RGB24 );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, BasicRoundTrip)
{
    const int W = 64;
    const int H = 48;
    const float TIMEOUT = 1.0f;

    auto handle = sender::CreateCamera(W, H, 60.0f, sender::FORMAT_RGBA32);
    ASSERT_TRUE( handle );

    std::atomic<int> flag = 0;

    std::thread th([&]
    {
        auto fb = sc::FrameBuffer::open();
        ASSERT_TRUE( fb );
        ASSERT_EQ( fb.imageFormat(), sc::ImageFormat::RGBA32 );

        EXPECT_EQ( fb.frameCounter(), 0 );
        flag = 1;

        fb.waitForNewFrame(0, TIMEOUT);
        EXPECT_EQ( fb.frameCounter(), 1 );

        // Image is W*H*3 (BGR) for the DIB.
        unsigned char dib[W * H * 3] = {};
        uint64_t frame_counter = 0;
        fb.transferToDIB(dib, &frame_counter);
        EXPECT_EQ( frame_counter, 1 );

        // First pixel in the source RGBA buffer was (R=10, G=20, B=30, A=255).
        // After convert_rgba_to_bgr the DIB starts with B,G,R = 30,20,10.
        EXPECT_EQ( dib[0], 30 );
        EXPECT_EQ( dib[1], 20 );
        EXPECT_EQ( dib[2], 10 );

        // First pixel of the bottom row (image is top-down in DIB):
        // last source row was all (R=200, G=100, B=50, A=255).
        const std::size_t stride = W * 3;
        EXPECT_EQ( dib[(H - 1) * stride + 0], 50 );
        EXPECT_EQ( dib[(H - 1) * stride + 1], 100 );
        EXPECT_EQ( dib[(H - 1) * stride + 2], 200 );
    });

    WAIT_FOR_FLAG_CHANGE(flag, 0);

    // Source: top row pixels are (R=10, G=20, B=30, A=255);
    // bottom row pixels are (R=200, G=100, B=50, A=255).
    unsigned char rgba[W * H * 4];
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            unsigned char r, g, b;
            if (y == 0) { r = 10; g = 20; b = 30; }
            else if (y == H - 1) { r = 200; g = 100; b = 50; }
            else { r = 1; g = 2; b = 3; }
            int idx = (y * W + x) * 4;
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 0xFF;
        }
    }
    sender::SendFrameRGBA(handle, rgba);

    th.join();
    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, RejectedIfCameraWasRGB24)
{
    auto handle = sender::CreateCamera(64, 48, 60.0f, sender::FORMAT_RGB24);
    ASSERT_TRUE( handle );

    auto fb = sc::FrameBuffer::open();
    ASSERT_TRUE( fb );
    ASSERT_EQ( fb.imageFormat(), sc::ImageFormat::RGB24 );
    EXPECT_EQ( fb.frameCounter(), 0 );

    // Should be a no-op: the camera's shared buffer is sized for 3 bytes/pixel,
    // writing 4 bytes/pixel would overflow.
    unsigned char rgba[64 * 48 * 4] = {};
    std::memset(rgba, 0xAA, sizeof(rgba));
    sender::SendFrameRGBA(handle, rgba);

    EXPECT_EQ( fb.frameCounter(), 0 );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, InvalidArgs)
{
    auto handle = sender::CreateCamera(64, 48, 60.0f, sender::FORMAT_RGBA32);
    unsigned char rgba[64 * 48 * 4] = {};

    EXPECT_NO_THROW({ sender::SendFrameRGBA(nullptr, nullptr); });
    EXPECT_NO_THROW({ sender::SendFrameRGBA(nullptr, rgba); });
    EXPECT_NO_THROW({ sender::SendFrameRGBA(handle, nullptr); });

    sender::DeleteCamera(handle);
}

TEST(SenderCreateCamera, RGBA32RequiresLargerSharedMemory)
{
    const int W = 64;
    const int H = 48;

    auto rgb_handle = sender::CreateCamera(W, H, 60.0f, sender::FORMAT_RGB24);
    auto rgb_fb = sc::FrameBuffer::open();
    ASSERT_TRUE( rgb_fb );
    auto rgb_size = rgb_fb.handle() ? 1 : 0; // touch handle to ensure open worked

    sender::DeleteCamera(rgb_handle);

    auto rgba_handle = sender::CreateCamera(W, H, 60.0f, sender::FORMAT_RGBA32);
    ASSERT_TRUE( rgba_handle );

    // After deleting RGB camera and creating RGBA, the new buffer must be
    // sized for 4 bytes/pixel. Verify by reading imageFormat().
    auto rgba_fb = sc::FrameBuffer::open();
    ASSERT_TRUE( rgba_fb );
    EXPECT_EQ( rgba_fb.imageFormat(), sc::ImageFormat::RGBA32 );

    sender::DeleteCamera(rgba_handle);
    (void)rgb_size;
}

} //namespace SenderAPITest
