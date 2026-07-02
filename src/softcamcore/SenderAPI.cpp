#include "SenderAPI.h"

#include <atomic>
#include <cstring>
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
    if (auto fb = FrameBuffer::create(width, height, framerate))
    {
        Camera* camera = new Camera{ fb, Timer() };
        Camera* expected = nullptr;
        if (s_camera.compare_exchange_strong(expected, camera))
        {
            return camera;
        }
        delete camera;
    }
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

// Throttle frame delivery to the configured framerate. Shared by both
// SendFrame and SendFrameRGBA so behavior is identical regardless of which
// pixel format the caller is using.
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

void            SendFrame(CameraHandle camera, const void* image_bits)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.load() == target && image_bits)
    {
        auto framerate = target->m_frame_buffer.framerate();
        auto frame_counter = target->m_frame_buffer.frameCounter();

        throttle(*target, framerate, frame_counter);

        target->m_frame_buffer.write(image_bits);
    }
}

void            SendFrameRGBA(CameraHandle camera, const void* rgba_bits)
{
    Camera* target = static_cast<Camera*>(camera);
    if (target && s_camera.load() == target && rgba_bits)
    {
        auto framerate = target->m_frame_buffer.framerate();
        auto frame_counter = target->m_frame_buffer.frameCounter();
        int w = target->m_frame_buffer.width();
        int h = target->m_frame_buffer.height();
        std::size_t pixel_count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

        throttle(*target, framerate, frame_counter);

        // Convert RGBA (top-down, byte order R,G,B,A) to BGR (top-down,
        // byte order B,G,R) -- the layout transferToDIB expects to find
        // verbatim in shared memory. SimdConvert picks SSE2/AVX2 when
        // available, scalar fallback otherwise.
        //
        // We allocate a full-frame scratch buffer (w*h*3 bytes) so the
        // write() lock is acquired exactly once, matching SendFrame
        // semantics. For 1920x1080 this is ~6 MB per call, which we
        // re-use across calls via a thread_local to avoid the allocation
        // churn.
        static thread_local std::vector<uint8_t> scratch;
        std::size_t need = pixel_count * 3;
        if (scratch.size() < need) scratch.resize(need);
        convert_rgba_to_bgr(
            static_cast<const uint8_t*>(rgba_bits),
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
