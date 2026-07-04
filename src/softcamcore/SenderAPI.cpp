#include "SenderAPI.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <vector>

#include "FrameBuffer.h"
#include "Misc.h"
#include "SimdConvert.h"


namespace {

struct Camera
{
    softcam::FrameBuffer    m_frame_buffer;
    softcam::Timer          m_timer;
};

std::atomic<Camera*>    s_camera;

} //namespace


namespace softcam {
namespace sender {

CameraHandle    CreateCamera(int width, int height, float framerate)
{
    auto fb = FrameBuffer::create(width, height, framerate);
    fprintf(stderr, "[softcam] FrameBuffer::create result=%s handle=%p\n", fb ? "OK" : "FAILED", fb.handle());
    fflush(stderr);
    if (fb)
    {
        Camera* camera = new Camera{ fb, Timer() };
        Camera* expected = nullptr;
        if (s_camera.compare_exchange_strong(expected, camera))
        {
            fprintf(stderr, "[softcam] CreateCamera OK handle=%p\n", camera);
            fflush(stderr);
            return camera;
        }
        delete camera;
        fprintf(stderr, "[softcam] CreateCamera FAILED: another camera already registered\n");
        fflush(stderr);
    }
    fprintf(stderr, "[softcam] CreateCamera FAILED\n");
    fflush(stderr);
    return nullptr;
}

void            DeleteCamera(CameraHandle camera)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.compare_exchange_strong(target, nullptr))
    {
        target->m_frame_buffer.deactivate();
        delete target;
    }
}

namespace {

// Throttle frame delivery to the configured framerate.
void throttle(Camera& target, float framerate, uint64_t frame_counter)
{
    if (0.0f >= framerate) return;

    if (0 == frame_counter)
    {
        target.m_timer.reset();
    }
    else
    {
        auto ref_delta = 1.0f / framerate;
        auto time = target.m_timer.get();
        if (time < ref_delta)
        {
            Timer::sleep(ref_delta - time);
        }
        if (time < ref_delta * 1.5f)
        {
            target.m_timer.rewind(ref_delta);
        }
        else
        {
            target.m_timer.reset();
        }
    }
}

} //namespace

// Sender-side entry point.
//
// Dart's `ui.Image.toByteData(format: rawRgba)` returns bytes in
//   [R, G, B, A, R, G, B, A, ...]
// byte order, top-down. The v3 shared-memory wire is BGRA32
//   [B, G, R, A, B, G, R, A, ...]
// which matches what DirectShow consumers expect on little-endian x86/x64.
//
// We convert in-place into a thread_local scratch buffer (one full frame
// at a time) using the SSSE3 byte shuffle from SimdConvert.h, then ship
// the BGRA32 bytes to FrameBuffer::write which memcpy's them into shmem
// under the named mutex. For 1920x1080 this is ~8 MB scratch -- the
// thread_local avoids calloc/free churn on the hot path.
//
// Receivers that open() FrameBuffer validate m_bpp == 4 and reject any
// other wire layout (hard protocol upgrade).
void            SendFrameRGBA(CameraHandle camera, const void* rgba_bits)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.load() == target && rgba_bits)
    {
        auto framerate = target->m_frame_buffer.framerate();
        auto frame_counter = target->m_frame_buffer.frameCounter();

        throttle(*target, framerate, frame_counter);

        const std::size_t pixel_count = static_cast<std::size_t>(target->m_frame_buffer.width())
                                      * static_cast<std::size_t>(target->m_frame_buffer.height());
        static thread_local std::vector<std::uint8_t> scratch;
        if (scratch.size() < pixel_count * 4) scratch.resize(pixel_count * 4);
        softcam::convert_rgba_to_bgra(
            static_cast<const std::uint8_t*>(rgba_bits),
            scratch.data(),
            pixel_count);
        target->m_frame_buffer.write(scratch.data());
    }
}

bool            WaitForConnection(CameraHandle camera, float timeout)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.load() == target)
    {
        Timer timer;
        while (!target->m_frame_buffer.connected())
        {
            if (0.0f < timeout && timeout <= timer.get())
            {
                return false;
            }
            Timer::sleep(0.001f);
        }
        return true;
    }
    return false;
}

bool            IsConnected(CameraHandle camera)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.load() == target)
    {
        return target->m_frame_buffer.connected();
    }
    return false;
}

} //namespace sender
} //namespace softcam
