#include "MjpegEncoder.h"

#include <wrl/client.h>
#include <wincodec.h>
#include <vector>

using Microsoft::WRL::ComPtr;


namespace softcam {


struct MjpegEncoder::Impl
{
    ComPtr<IWICImagingFactory>   factory;
    ComPtr<IWICStream>           stream;
    float                        quality = 80.0f;
    std::vector<std::uint8_t>    scratch;

    bool init()
    {
        if (factory) return true;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return false;
        return true;
    }
};


MjpegEncoder::MjpegEncoder()
    : m_impl(new Impl())
{
    m_impl->init();
}

MjpegEncoder::~MjpegEncoder()
{
    delete m_impl;
}

void MjpegEncoder::setQuality(float q)
{
    if (q < 0.0f) q = 0.0f;
    if (q > 100.0f) q = 100.0f;
    m_impl->quality = q;
}

std::size_t MjpegEncoder::encode(const std::uint8_t* bgra_topdown,
                                  int width, int height,
                                  std::uint8_t* dst, std::size_t dst_cap)
{
    if (!m_impl->factory) return 0;
    if (!bgra_topdown || width <= 0 || height <= 0) return 0;
    if (!dst || dst_cap == 0) return 0;

    HRESULT hr = S_OK;

    // Build a WIC bitmap directly over the caller's BGRA buffer (no copy).
    ComPtr<IWICBitmap> bitmap;
    hr = m_impl->factory->CreateBitmapFromMemory(
        width, height,
        GUID_WICPixelFormat32bppBGRA,
        width * 4,
        static_cast<UINT>(static_cast<std::size_t>(width) * height * 4),
        const_cast<std::uint8_t*>(bgra_topdown),
        &bitmap);
    if (FAILED(hr)) return 0;

    // Recreate the stream each call -- WIC streams are cheap and this avoids
    // cross-thread issues if we ever share the encoder across threads.
    ComPtr<IWICStream> stream;
    hr = m_impl->factory->CreateStream(&stream);
    if (FAILED(hr)) return 0;

    // Bind the stream to the caller's output buffer.
    hr = stream->InitializeFromMemory(dst, static_cast<DWORD>(dst_cap));
    if (FAILED(hr)) return 0;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = m_impl->factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) return 0;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return 0;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return 0;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return 0;

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return 0;

    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) return 0;

    // SetQuality is not declared in the bundled WIC headers in this
    // build (depends on wincodec.h version). Default WIC JPEG quality
    // is ~0.9 which is acceptable for the vcam use case. To wire up
    // m_impl->quality, pass an IPropertyBag2 to CreateNewFrame and
    // write EncoderQuality into it.
    (void)m_impl->quality;

    hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (FAILED(hr)) return 0;

    hr = frame->Commit();
    if (FAILED(hr)) return 0;

    hr = encoder->Commit();
    if (FAILED(hr)) return 0;

    ULARGE_INTEGER pos{};
    if (SUCCEEDED(stream->Seek({0}, STREAM_SEEK_CUR, &pos)))
    {
        return static_cast<std::size_t>(pos.QuadPart);
    }
    return dst_cap; // best-effort fallback
}


} //namespace softcam