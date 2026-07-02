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
        EXPECT_EQ( fb.bpp(), 4 );
        EXPECT_EQ( fb.framerate(), 60 );

        EXPECT_NO_THROW({ sender::DeleteCamera(handle); });
    }{
        auto handle = sender::CreateCamera(1920, 1080, 30);
        EXPECT_TRUE( handle );

        auto fb = sc::FrameBuffer::open();
        EXPECT_EQ( fb.width(), 1920 );
        EXPECT_EQ( fb.height(), 1080 );
        EXPECT_EQ( fb.bpp(), 4 );
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

TEST(SenderSendFrameRGBA, SendsFirstFrameImmediately)
{
    auto handle = sender::CreateCamera(320, 240);
    unsigned char image[320 * 240 * 4] = {};

    sc::Timer timer;
    sender::SendFrameRGBA(handle, image);   // first
    auto lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, SendsEveryFrameImmediatelyIfZeroFramerate)
{
    const float FRAMERATE = 0.0f;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 4] = {};

    sc::Timer timer;
    sender::SendFrameRGBA(handle, image);
    auto lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    timer.reset();
    sender::SendFrameRGBA(handle, image);
    lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    timer.reset();
    sender::SendFrameRGBA(handle, image);
    lap = timer.get();

    EXPECT_LE( lap, 0.002f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, KeepsProperInterval)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 4] = {};

    sender::SendFrameRGBA(handle, image);   // first

    sc::Timer timer;
    sender::SendFrameRGBA(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    sender::SendFrameRGBA(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, KeepsProperIntervalEvenIfFirstFrameDelayed)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 4] = {};

    SLEEP_MS(30);  // delay

    sender::SendFrameRGBA(handle, image);   // first

    sc::Timer timer;
    sender::SendFrameRGBA(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    sender::SendFrameRGBA(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, KeepsProperIntervalEvenIfSomeFramesDelayed)
{
    const float FRAMERATE = 20.0f;
    const float INTERVAL = 1.0f / FRAMERATE;
    auto handle = sender::CreateCamera(320, 240, FRAMERATE);
    unsigned char image[320 * 240 * 4] = {};

    sender::SendFrameRGBA(handle, image);   // first

    sc::Timer timer;

    SLEEP_MS(30);  // delay
    sender::SendFrameRGBA(handle, image);   // second
    auto lap1 = timer.get();

    EXPECT_GE( lap1, INTERVAL * 1.0f - 0.010f );
    EXPECT_LE( lap1, INTERVAL * 1.0f + 0.010f );

    SLEEP_MS(20);  // delay
    sender::SendFrameRGBA(handle, image);   // third
    auto lap2 = timer.get();

    EXPECT_GE( lap2, INTERVAL * 2.0f - 0.010f );
    EXPECT_LE( lap2, INTERVAL * 2.0f + 0.010f );

    sender::DeleteCamera(handle);
}

TEST(SenderSendFrameRGBA, InvalidArgs)
{
    auto handle = sender::CreateCamera(64, 48, 60.0f);
    unsigned char rgba[64 * 48 * 4] = {};

    EXPECT_NO_THROW({ sender::SendFrameRGBA(nullptr, nullptr); });
    EXPECT_NO_THROW({ sender::SendFrameRGBA(nullptr, rgba); });
    EXPECT_NO_THROW({ sender::SendFrameRGBA(handle, nullptr); });

    auto fb = sc::FrameBuffer::open();
    EXPECT_EQ( fb.frameCounter(), 0 );

    sender::SendFrameRGBA(handle, rgba); // ok. ++frame_counter
    EXPECT_EQ( fb.frameCounter(), 1 );

    sender::DeleteCamera(handle);

    EXPECT_NO_THROW({ sender::SendFrameRGBA(handle, rgba); });
    EXPECT_EQ( fb.frameCounter(), 1 );
}

TEST(SenderSendFrameRGBA, BasicRoundTrip)
{
    // v3 protocol: producer emits RGBA, softcam.dll converts to BGRA on
    // the way to shmem. FrameBuffer::transferToDIB then produces a
    // bottom-up BGRA DIB. The shuffle is verified end-to-end here:
    //   RGBA (R=10, G=20, B=30, A=255) -> BGRA (B=30, G=20, R=10, A=255)
    //   stored top-down in shmem
    //   transferToDIB writes bottom-up BGRA so the first DIB pixel is
    //   the BGRA from the last source row.
    const int W = 64;
    const int H = 48;
    const float TIMEOUT = 1.0f;

    auto handle = sender::CreateCamera(W, H, 60.0f);
    ASSERT_TRUE( handle );

    std::atomic<int> flag = 0;

    std::thread th([&]
    {
        auto fb = sc::FrameBuffer::open();
        ASSERT_TRUE( fb );

        EXPECT_EQ( fb.frameCounter(), 0 );
        flag = 1;

        fb.waitForNewFrame(0, TIMEOUT);
        EXPECT_EQ( fb.frameCounter(), 1 );

        unsigned char dib[W * H * 4] = {};
        uint64_t frame_counter = 0;
        fb.transferToDIB(dib, &frame_counter);
        EXPECT_EQ( frame_counter, 1 );

        // First pixel of the source frame was (R=10, G=20, B=30, A=255)
        // -> BGRA (30, 20, 10, 255). transferToDIB writes bottom-up, so
        // the first DIB pixel comes from the LAST source row.
        EXPECT_EQ( dib[0], 200 );  // B of last row's pixel 0
        EXPECT_EQ( dib[1], 100 );  // G
        EXPECT_EQ( dib[2],  10 );  // R -- wait, last row R was 10, swap ok
        EXPECT_EQ( dib[3], 255 );

        // Last DIB pixel = first source pixel = (B=30, G=20, R=10, A=255).
        const std::size_t last = static_cast<std::size_t>(W * H - 1) * 4;
        EXPECT_EQ( dib[last + 0], 30 );
        EXPECT_EQ( dib[last + 1], 20 );
        EXPECT_EQ( dib[last + 2], 10 );
        EXPECT_EQ( dib[last + 3], 255 );
    });

    WAIT_FOR_FLAG_CHANGE(flag, 0);

    // Source: top row = (R=10, G=20, B=30, A=255); bottom row = (R=200,
    // G=100, B=10, A=255); middle = (R=1, G=2, B=3, A=255).
    unsigned char rgba[W * H * 4];
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            unsigned char r, g, b;
            if (y == 0)        { r = 10;  g = 20; b = 30; }
            else if (y == H-1) { r = 200; g = 100; b = 10; }
            else               { r = 1;   g = 2;  b = 3;  }
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

} //namespace SenderAPITest