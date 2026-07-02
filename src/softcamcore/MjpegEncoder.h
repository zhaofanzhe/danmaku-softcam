#pragma once

#include <cstdint>
#include <cstddef>


namespace softcam {


// WIC-backed Motion JPEG encoder.
//
// Owns its own IWICImagingFactory and stream pool. encode() takes a
// top-down BGRA32 frame and writes a JPEG byte stream into dst. Returns
// the number of bytes written, or 0 on failure.
//
// Threading: not thread-safe; one instance per DirectShow streaming
// thread is sufficient.
class MjpegEncoder
{
public:
    MjpegEncoder();
    ~MjpegEncoder();

    // Set the JPEG quality level (0..100, where 100 is best). Default 80.
    // Cheap to call -- just caches a float for the next encode().
    void setQuality(float q);

    // Encode a top-down BGRA32 frame into dst (capacity = dst_cap).
    // Returns the JPEG payload size in bytes, or 0 on error.
    std::size_t encode(const std::uint8_t* bgra_topdown,
                       int width, int height,
                       std::uint8_t* dst, std::size_t dst_cap);

private:
    struct Impl;
    Impl* m_impl;
};


} //namespace softcam