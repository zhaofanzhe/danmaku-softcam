#include "FrameBuffer.h"

#include <windows.h>
#include <mutex> // lock_guard
#include <cstring>

#include "SimdConvert.h"


namespace softcam {


const char NamedMutexName[] = "DirectShow Softcam/NamedMutex";
const char SharedMemoryName[] = "DirectShow Softcam/SharedMemory";
// Bumped from 2 to 3 when RGBA32 ImageFormat support was added.
// Receivers/senders built against ProtocolVersion < 3 cannot safely interoperate
// when the shared buffer is in RGBA32 format (they assume RGB24 layout).
const uint8_t ProtocolVersion = 3;


struct FrameBuffer::Header
{
    uint32_t    m_image_offset;
    uint16_t    m_width;
    uint16_t    m_height;
    float       m_framerate;
    uint8_t     m_is_active;
    uint8_t     m_connected_min_version; // 0 or 1 or 2 or 3
    uint8_t     m_watchdog_sender_heartbeat;
    uint8_t     m_watchdog_receiver_heartbeat;
    uint64_t    m_frame_counter;
    uint8_t     m_image_format;   // ImageFormat enum value

    uint8_t*    imageData();
};


uint8_t* FrameBuffer::Header::imageData()
{
    uint8_t *image = reinterpret_cast<uint8_t*>(this) + m_image_offset;
    return image;
}


FrameBuffer FrameBuffer::create(
                        int             width,
                        int             height,
                        float           framerate,
                        ImageFormat     format)
{
    FrameBuffer fb(NamedMutexName);

    if (!checkDimensions(width, height))
    {
        return fb;
    }
    if (framerate < 0.0f)
    {
        return fb;
    }

    auto shmem_size = calcMemorySize((uint16_t)width, (uint16_t)height, format);
    fb.m_shmem = SharedMemory::create(SharedMemoryName, shmem_size);
    if (fb.m_shmem)
    {
        std::lock_guard<NamedMutex> lock(fb.m_mutex);

        auto frame = fb.header();
        frame->m_image_offset = sizeof(Header);
        frame->m_width = (uint16_t)width;
        frame->m_height = (uint16_t)height;
        frame->m_framerate = framerate;
        frame->m_is_active = 1;
        frame->m_connected_min_version = 0;
        frame->m_watchdog_sender_heartbeat = 0;
        frame->m_watchdog_receiver_heartbeat = 0;
        frame->m_frame_counter = 0;
        frame->m_image_format = static_cast<uint8_t>(format);

        auto mutex = fb.m_mutex;
        fb.m_sender_watchdog = Watchdog::createHeartbeat(
            WATCHDOG_HEARTBEAT_INTERVAL,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                frame->m_watchdog_sender_heartbeat += 1;
            });
        fb.m_receiver_watchdog = Watchdog::createMonitor(
            WATCHDOG_MONITOR_INTERVAL,
            WATCHDOG_TIMEOUT,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                return frame->m_watchdog_receiver_heartbeat;
            });
    }
    return fb;
}

FrameBuffer FrameBuffer::open()
{
    FrameBuffer fb(NamedMutexName);

    fb.m_shmem = SharedMemory::open(SharedMemoryName);
    if (fb.m_shmem)
    {
        std::lock_guard<NamedMutex> lock(fb.m_mutex);

        auto size = fb.m_shmem.size();
        if (size < sizeof(Header))
        {
            fb.m_shmem = {};
            return fb;
        }
        auto frame = fb.header();
        if (!checkDimensions(frame->m_width, frame->m_height) ||
            frame->m_framerate < 0.0f)
        {
            fb.m_shmem = {};
            return fb;
        }
        auto fmt = static_cast<ImageFormat>(frame->m_image_format);
        // For RGBA32 the sender must be ProtocolVersion 3+; older build artifacts
        // may not have populated m_image_format at all, in which case it reads as 0
        // (RGB24) by luck -- explicit validation is unnecessary because the size
        // check below rejects any format/size mismatch.
        uint32_t image_size = (uint32_t)frame->m_width * (uint32_t)frame->m_height
                              * (uint32_t)bytesPerPixel(fmt);
        if (size <= frame->m_image_offset ||
            size - frame->m_image_offset < image_size)
        {
            fb.m_shmem = {};
            return fb;
        }

        auto mutex = fb.m_mutex;
        fb.m_sender_watchdog = Watchdog::createMonitor(
            WATCHDOG_MONITOR_INTERVAL,
            WATCHDOG_TIMEOUT,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                return frame->m_watchdog_sender_heartbeat;
            });
        fb.m_receiver_watchdog = Watchdog::createHeartbeat(
            WATCHDOG_HEARTBEAT_INTERVAL,
            [mutex, frame]() mutable
            {
                std::lock_guard<NamedMutex> lock(mutex);
                frame->m_watchdog_receiver_heartbeat += 1;
            });
        if (0 == frame->m_connected_min_version ||
            ProtocolVersion <= frame->m_connected_min_version)
        {
            frame->m_connected_min_version = ProtocolVersion;
        }
        frame->m_watchdog_receiver_heartbeat += 1;
    }

    return fb;
}

FrameBuffer&
FrameBuffer::operator =(const FrameBuffer& fb)
{
    m_receiver_watchdog = {};
    m_sender_watchdog = {};
    m_shmem = {};
    m_shmem = fb.m_shmem;
    m_sender_watchdog = fb.m_sender_watchdog;
    m_receiver_watchdog = fb.m_receiver_watchdog;
    return *this;
}

void* FrameBuffer::handle() const
{
    return const_cast<void*>(m_shmem.get());
}

int FrameBuffer::width() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_width : 0;
}

int FrameBuffer::height() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_height : 0;
}

float FrameBuffer::framerate() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_framerate : 0.0f;
}

uint64_t FrameBuffer::frameCounter() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem ? header()->m_frame_counter : 0;
}

ImageFormat FrameBuffer::imageFormat() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    if (!m_shmem) return ImageFormat::RGB24;
    auto raw = header()->m_image_format;
    return raw == static_cast<uint8_t>(ImageFormat::RGBA32)
               ? ImageFormat::RGBA32
               : ImageFormat::RGB24;
}

bool FrameBuffer::active() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    return m_shmem && header()->m_is_active;
}

bool FrameBuffer::connected() const
{
    std::lock_guard<NamedMutex> lock(m_mutex);
    if (m_shmem)
    {
        auto ver = header()->m_connected_min_version;
        if (0 == ver)
        {
            // No receivers connected
            return false;
        }
        if (1 == ver)
        {
            // At least one of the connected receivers is version 1.
            // Since receivers of version 1 don't have receiver-watchdog,
            // we won't know their disconnection.
            return true;
        }
        return m_receiver_watchdog.alive();
    }
    return false;
}

void FrameBuffer::deactivate()
{
    if (!m_shmem) return;
    std::lock_guard<NamedMutex> lock(m_mutex);
    header()->m_is_active = 0;
}

void FrameBuffer::write(const void* image_bits)
{
    if (!m_shmem) return;
    std::lock_guard<NamedMutex> lock(m_mutex);
    auto frame = header();
    auto fmt = static_cast<ImageFormat>(frame->m_image_format);
    std::size_t bytes = (std::size_t)frame->m_width * frame->m_height
                        * bytesPerPixel(fmt);
    std::memcpy(frame->imageData(), image_bits, bytes);
    frame->m_frame_counter += 1;
}

void FrameBuffer::transferToDIB(void* image_bits, uint64_t* out_frame_counter)
{
    if (!m_shmem)
    {
        *out_frame_counter = 0;
        return;
    }
    std::lock_guard<NamedMutex> lock(m_mutex);

    auto frame = header();
    {
        int w = frame->m_width;
        int h = frame->m_height;
        int gap = ((w * 3 + 3) & ~3) - w * 3;
        const std::uint8_t* image = frame->imageData();
        std::uint8_t* dest = (std::uint8_t*)image_bits;
        auto fmt = static_cast<ImageFormat>(frame->m_image_format);
        if (fmt == ImageFormat::RGBA32)
        {
            // Convert RGBA -> BGR (DIB RGB24) per row, then flip vertically.
            // convert_rgba_to_bgr is SSE2/AVX2 when available, scalar otherwise.
            for (int y = 0; y < h; y++)
            {
                const std::uint8_t* src_row =
                    image + 4 * w * (h - 1 - y);
                convert_rgba_to_bgr(src_row, dest, (std::size_t)w);
                dest += 3 * w + gap;
            }
        }
        else
        {
            for (int y = 0; y < h; y++)
            {
                const std::uint8_t* src = image + 3 * w * (h - 1 - y);
                std::memcpy(dest, src, 3 * (uint32_t)w);
                dest += 3 * w + gap;
            }
        }
        *out_frame_counter = frame->m_frame_counter;
    }
}

bool FrameBuffer::waitForNewFrame(uint64_t frame_counter, float time_out)
{
    if (!m_shmem) return false;
    Timer timer;
    while (active() && m_sender_watchdog.alive())
    {
        if (frameCounter() > frame_counter)
        {
            return true;
        }
        Timer::sleep(0.001f);
        if (0.0f < time_out && time_out <= timer.get())
        {
            return true;
        }
    }
    return false;
}

void FrameBuffer::release()
{
    m_receiver_watchdog.stop();
    m_sender_watchdog.stop();
    m_shmem = SharedMemory{};
}

FrameBuffer::Header* FrameBuffer::header()
{
    return static_cast<Header*>(m_shmem.get());
}

const FrameBuffer::Header* FrameBuffer::header() const
{
    return static_cast<const Header*>(m_shmem.get());
}

bool FrameBuffer::checkDimensions(
                        int width,
                        int height)
{
    // We reject big numbers and numbers not multiple of four.
    // Too large pictures may exceed the 32-bit limit of DIBitmap and shared memory.
    // Sizes which are not multiple of four tend to cause problems in the subsequent
    // processes such as color conversion or compression in video applications.
    if (width < 1 || width > 16384 || width % 4 != 0 ||
        height < 1 || height > 16384 || height % 4 != 0)
    {
        return false;
    }
    return true;
}

uint32_t FrameBuffer::calcMemorySize(
                        uint16_t width,
                        uint16_t height,
                        ImageFormat format)
{
    uint32_t header_size = sizeof(Header);
    uint32_t image_size = (uint32_t)width * height
                          * (uint32_t)bytesPerPixel(format);
    uint32_t shmem_size = header_size + image_size;
    return shmem_size;
}


} //namespace softcam
