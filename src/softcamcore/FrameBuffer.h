#pragma once

#include <cstdint>
#include <cstddef>
#include "Misc.h"
#include "Watchdog.h"


namespace softcam {


using std::uint64_t;
using std::uint32_t;
using std::uint16_t;


/// Pixel format of the image data stored in the shared frame buffer.
///
/// The Sender writes pixels in this format; the Receiver reads them in the
/// same format. DirectShow consumers always receive RGB24 in the DIB
/// (BI_RGB / MEDIASUBTYPE_RGB24) regardless of the shared buffer format --
/// `transferToDIB` performs the conversion if the source format is RGBA32.
enum class ImageFormat : uint8_t
{
    RGB24  = 0,
    RGBA32 = 1,
};

inline constexpr std::size_t bytesPerPixel(ImageFormat fmt) noexcept
{
    return fmt == ImageFormat::RGBA32 ? std::size_t{4} : std::size_t{3};
}


/// Shared frame buffer between processes (sender and receiver)
class FrameBuffer
{
 public:
    static FrameBuffer create(
                        int             width,
                        int             height,
                        float           framerate = 0.0f,
                        ImageFormat     format = ImageFormat::RGB24);
    static FrameBuffer open();

    FrameBuffer& operator =(const FrameBuffer&);
    explicit operator bool() const { return handle() != nullptr; }

    void*           handle() const;
    int             width() const;
    int             height() const;
    float           framerate() const;
    uint64_t        frameCounter() const;
    ImageFormat     imageFormat() const;
    bool            active() const;
    bool            connected() const;

    void            deactivate();
    void            write(const void* image_bits);
    void            transferToDIB(void* image_bits, uint64_t* out_frame_counter);
    bool            waitForNewFrame(uint64_t frame_counter, float time_out = 0.5f);

    void            release();

    static constexpr float WATCHDOG_HEARTBEAT_INTERVAL = 0.02f;
    static constexpr float WATCHDOG_MONITOR_INTERVAL = 0.02f;
    static constexpr float WATCHDOG_TIMEOUT = 0.5f;

 private:
    struct Header;

    mutable NamedMutex      m_mutex;
    SharedMemory            m_shmem;
    Watchdog                m_sender_watchdog;
    Watchdog                m_receiver_watchdog;

    explicit FrameBuffer(const char* mutex_name) : m_mutex(mutex_name) {}

    Header*         header();
    const Header*   header() const;

    static bool     checkDimensions(
                        int width,
                        int height);
    static uint32_t calcMemorySize(
                        uint16_t width,
                        uint16_t height,
                        ImageFormat format);
};


} //namespace softcam
