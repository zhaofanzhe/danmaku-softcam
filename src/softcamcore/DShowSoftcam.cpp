#include "DShowSoftcam.h"

#include <cstring>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <ctime>

#include "MjpegEncoder.h"
#include "SimdConvert.h"


namespace {

using SubtypeInfo = ::softcam::SubtypeInfo;

//#define ENABLE_LOG
//#define LOG_FILE_PATH "C:\\my_temp\\debug_log.txt"

#if defined(ENABLE_LOG)

FILE* logfile = nullptr;
#define OPEN_LOGFILE() logfile = std::fopen(LOG_FILE_PATH, "a")
#define CLOSE_LOGFILE() [&]{ \
        if (logfile) std::fclose(logfile); \
        logfile = nullptr; \
    }()
#define NOW() []{ \
        auto tp = std::chrono::system_clock::now(); \
        auto t = std::chrono::system_clock::to_time_t(tp); \
        static char buff[128]; \
        std::strftime(buff, sizeof(buff), "%H:%M:%S", std::localtime(&t)); \
        return buff; \
    }()
std::string IID_TO_STR(REFIID riid)
{
    std::string s =
            riid == IID_IPin                ? "IPin" :
            riid == IID_IBaseFilter         ? "IBaseFilter" :
            riid == IID_IAMovieSetup        ? "IAMovieSetup" :
            riid == IID_IQualityControl     ? "IQualityControl" :
            riid == IID_IAMStreamConfig     ? "IAMStreamConfig" :
            riid == IID_IKsPropertySet      ? "IKsPropertySet" :
            riid == IID_IAMFilterMiscFlags  ? "IAMFilterMiscFlags" :
            riid == IID_IPersistPropertyBag ? "IPersistPropertyBag" :
            riid == IID_IReferenceClock     ? "IReferenceClock" :
            riid == IID_IMediaSeeking       ? "IMediaSeeking" :
            riid == IID_IAMDeviceRemoval    ? "IAMDeviceRemoval" :
            riid == IID_IAMOpenProgress     ? "IAMOpenProgress" :
            riid == IID_IMediaPosition      ? "IMediaPosition" :
            riid == IID_IMediaFilter        ? "IMediaFilter" :
            riid == IID_IBasicVideo         ? "IBasicVideo" :
            riid == IID_IBasicAudio         ? "IBasicAudio" :
            riid == IID_IVideoWindow        ? "IVideoWindow" :
            riid == IID_IUnknown            ? "IUnknown" :
            [&]() -> std::string
            {
                char buff[128];
                snprintf(buff, sizeof(buff),
                    "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                    riid.Data1, riid.Data2, riid.Data3,
                    riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                    riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
                return buff;
            }();
    return s;
}
#define LOG(fmt, ...) [&,funcname=__func__]{ \
        if (logfile) std::fprintf(logfile, "%s: %s " fmt, NOW(), funcname, __VA_ARGS__); \
        std::printf("%s: %s " fmt, NOW(), funcname, __VA_ARGS__); \
    }()

#else // ENABLE_LOG

#define OPEN_LOGFILE()
#define CLOSE_LOGFILE()
#define LOG(...)

#endif // ENABLE_LOG


// -------------------------------------------------------------------------
// Subtype table
// -------------------------------------------------------------------------
//
// The order here is the order consumers see via IAMStreamConfig::GetStreamCaps.
// Many capture graphs (OBS, Chrome getUserMedia, vMix) negotiate the FIRST
// subtype they support, so ARGB32 sits at index 0 -- it's the only RGB
// variant that carries alpha and the one we want alpha-aware consumers to
// pick.
//
// YUV family follows in 4:2:2 then 4:2:0 order, then monochrome, then MJPG.

static const SubtypeInfo kSubtypes[] = {
    // 32-bit RGB family. ARGB32 carries alpha; RGB32 is byte-identical in
    // memory but advertises as MEDIASUBTYPE_RGB32 so older capture graphs
    // (no ARGB awareness) still negotiate it.
    { &MEDIASUBTYPE_ARGB32, 32, BI_RGB,  0, 4, L"ARGB32 (BGRA)" },
    { &MEDIASUBTYPE_RGB32,  32, BI_RGB,  0, 4, L"RGB32 (BGRA)"  },

    // 4:2:2 packed YUV
    { &MEDIASUBTYPE_YUY2,   16, MAKEFOURCC('Y','U','Y','2'), mmioFOURCC('Y','U','Y','2'), 0, L"YUY2" },
    { &MEDIASUBTYPE_UYVY,   16, MAKEFOURCC('U','Y','V','Y'), mmioFOURCC('U','Y','V','Y'), 0, L"UYVY" },
    { &MEDIASUBTYPE_YVYU,   16, MAKEFOURCC('Y','V','Y','U'), mmioFOURCC('Y','V','Y','U'), 0, L"YVYU" },

    // 4:2:0 planar / semi-planar YUV
    { &MEDIASUBTYPE_IYUV,   12, MAKEFOURCC('I','Y','U','V'), mmioFOURCC('I','Y','U','V'), 0, L"IYUV (I420)" },
    { &MEDIASUBTYPE_YV12,   12, MAKEFOURCC('Y','V','1','2'), mmioFOURCC('Y','V','1','2'), 0, L"YV12"        },
    { &MEDIASUBTYPE_NV12,   12, MAKEFOURCC('N','V','1','2'), mmioFOURCC('N','V','1','2'), 0, L"NV12"        },

    // JPEG (variable length, image size left to encoder)
    { &MEDIASUBTYPE_MJPG,    0, MAKEFOURCC('M','J','P','G'), mmioFOURCC('M','J','P','G'), 0, L"MJPG" },
};


// MJPG's biSizeImage is unknown at SetFormat time. We give consumers a
// conservative hint (a quarter of the BGRA32 frame size), which is enough
// to size the allocator pool without choking on actual larger JPEGs.
static std::uint32_t mjpgHintSize(int w, int h)
{
    return (static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h)) / 2u;
}


// -------------------------------------------------------------------------
// AM_MEDIA_TYPE helpers
// -------------------------------------------------------------------------

AM_MEDIA_TYPE* allocateMediaType()
{
    BYTE *pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!pbFormat)
    {
        return nullptr;
    }

    AM_MEDIA_TYPE *amt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!amt)
    {
        CoTaskMemFree(pbFormat);
        return nullptr;
    }
    std::memset(amt, 0, sizeof(*amt));
    amt->pbFormat = pbFormat;
    return amt;
}

std::uint32_t calcDIBSizeImpl(int width, int height, const SubtypeInfo& sub)
{
    // RGB-family uses 4-byte aligned rows. biSizeImage hint also has to
    // match the stride rounding for the capture graph's allocator sizing.
    if (sub.biCompression == BI_RGB && sub.biBitCount >= 8)
    {
        std::uint32_t row_bytes = (static_cast<std::uint32_t>(width) * sub.biBitCount + 7u) / 8u;
        std::uint32_t stride = (row_bytes + 3u) & ~3u;
        return stride * static_cast<std::uint32_t>(height);
    }

    switch (sub.biBitCount)
    {
        case 16: // 4:2:2 packed
            return static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * 2u;
        case 12: // 4:2:0 planar
            return static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * 3u / 2u;
        case 8:  // monochrome
            return static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
        case 0:  // MJPG / H264 -- variable length
            return mjpgHintSize(width, height);
        default:
            return 0;
    }
}

void fillMediaType(AM_MEDIA_TYPE* amt,
                   int width, int height, float framerate,
                   const SubtypeInfo& sub)
{
    BYTE *pbFormat = amt->pbFormat;

    if (framerate <= 0.0f)
    {
        framerate = 60.0f;
    }
    // bit_rate is informational; for variable-length codecs we just guess
    // a conservative average so upstream filters don't choke on dwBitRate=0.
    const float avg_bytes_per_frame = std::max<std::uint32_t>(calcDIBSizeImpl(width, height, sub), 1u);
    const float bit_rate = avg_bytes_per_frame * 8.0f * framerate;
    const float period = 10 * 1000 * 1000 / framerate;

    VIDEOINFOHEADER* pFormat = (VIDEOINFOHEADER*)pbFormat;
    std::memset(pFormat, 0, sizeof(*pFormat));
    pFormat->dwBitRate = (uint32_t)(std::min)(bit_rate, (float)INT_MAX);
    pFormat->dwBitErrorRate = 0;
    pFormat->AvgTimePerFrame = (LONGLONG)std::round((std::min)(period, (float)LONG_MAX));
    pFormat->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pFormat->bmiHeader.biWidth = width;
    pFormat->bmiHeader.biHeight = height;
    pFormat->bmiHeader.biPlanes = 1;
    pFormat->bmiHeader.biBitCount = sub.biBitCount;
    pFormat->bmiHeader.biCompression = sub.biCompression;
    pFormat->bmiHeader.biSizeImage = calcDIBSizeImpl(width, height, sub);

    amt->majortype = MEDIATYPE_Video;
    amt->subtype = *sub.subtype;
    amt->bFixedSizeSamples = (sub.biBitCount != 0);  // MJPG is VBR
    amt->bTemporalCompression = FALSE;
    amt->lSampleSize = static_cast<uint32_t>(calcDIBSizeImpl(width, height, sub));
    amt->formattype = FORMAT_VideoInfo;
    amt->pUnk = nullptr;
    amt->cbFormat = sizeof(VIDEOINFOHEADER);
    amt->pbFormat = pbFormat;
}

AM_MEDIA_TYPE* makeMediaType(int width, int height, float framerate,
                             const SubtypeInfo& sub)
{
    AM_MEDIA_TYPE *amt = allocateMediaType();
    if (!amt)
    {
        return nullptr;
    }
    fillMediaType(amt, width, height, framerate, sub);
    return amt;
}

} //namespace


// -------------------------------------------------------------------------
// SubtypeInfo API
// -------------------------------------------------------------------------

namespace softcam {

const SubtypeInfo* supportedSubtypes()
{
    return kSubtypes;
}

std::size_t supportedSubtypeCount()
{
    return sizeof(kSubtypes) / sizeof(kSubtypes[0]);
}

const SubtypeInfo* findSubtype(const GUID& subtype)
{
    for (std::size_t i = 0; i < supportedSubtypeCount(); ++i)
    {
        if (subtype == *kSubtypes[i].subtype) return &kSubtypes[i];
    }
    return nullptr;
}

std::uint32_t calcDIBSize(int width, int height, const SubtypeInfo& sub)
{
    return calcDIBSizeImpl(width, height, sub);
}


// -------------------------------------------------------------------------
// Softcam filter
// -------------------------------------------------------------------------

CUnknown * Softcam::CreateInstance(
                    LPUNKNOWN   lpunk,
                    const GUID& clsid,
                    HRESULT*    phr)
{
    OPEN_LOGFILE();
    LOG("===== logging started =====\n");

    return new Softcam(lpunk, clsid, phr);
}

Softcam::Softcam(LPUNKNOWN lpunk, const GUID& clsid, HRESULT *phr) :
    CSource(NAME("DirectShow Softcam"), lpunk, clsid),
    m_frame_buffer(FrameBuffer::open()),
    m_valid(m_frame_buffer ? true : false),
    m_width(m_frame_buffer.width()),
    m_height(m_frame_buffer.height()),
    m_bpp(m_frame_buffer.bpp()),
    m_framerate(m_frame_buffer.framerate())
{
    (void)new SoftcamStream(phr, this, L"DirectShow Softcam Stream");
}


STDMETHODIMP Softcam::NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv)
{
    if (riid == IID_IAMStreamConfig)
    {
        LOG("(Softcam) IAMStreamConfig -> S_OK\n");
        return GetInterface(static_cast<IAMStreamConfig*>(this), ppv);
    }
    else
    {
        auto result = CSource::NonDelegatingQueryInterface(riid, ppv);
        LOG("(Softcam) %s -> %s\n", IID_TO_STR(riid).c_str(), result ? "(ERROR)" : "S_OK");
        return result;
    }
}

HRESULT
Softcam::SetFormat(AM_MEDIA_TYPE *mt)
{
    if (!mt)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (!m_valid)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }
    if (mt->majortype != MEDIATYPE_Video)
    {
        LOG("-> E_FAIL (invalid major type)\n");
        return E_FAIL;
    }
    const SubtypeInfo* sub = findSubtype(mt->subtype);
    if (!sub)
    {
        LOG("-> E_FAIL (unsupported subtype)\n");
        return E_FAIL;
    }
    if (mt->formattype == FORMAT_VideoInfo && mt->pbFormat)
    {
        VIDEOINFOHEADER* pFormat = (VIDEOINFOHEADER*)mt->pbFormat;
        if (pFormat->bmiHeader.biWidth != m_width ||
            pFormat->bmiHeader.biHeight != m_height)
        {
            LOG("-> E_FAIL (invalid dimension)\n");
            return E_FAIL;
        }
        if (pFormat->bmiHeader.biCompression != sub->biCompression ||
            pFormat->bmiHeader.biBitCount != sub->biBitCount)
        {
            LOG("-> E_FAIL (invalid color format)\n");
            return E_FAIL;
        }
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT
Softcam::GetFormat(AM_MEDIA_TYPE **out_pmt)
{
    if (!out_pmt)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (!m_valid)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }
    // Default subtype is ARGB32 -- alpha-aware consumers see a real
    // transparent channel, legacy RGB32/YUV consumers re-negotiate via
    // GetStreamCaps.
    AM_MEDIA_TYPE* mt = makeMediaType(m_width, m_height, m_framerate, *findSubtype(MEDIASUBTYPE_ARGB32));
    if (!mt)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }
    *out_pmt = mt;
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT
Softcam::GetNumberOfCapabilities(int *out_count, int *out_size)
{
    if (!out_count || !out_size)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (!m_valid)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }
    *out_count = static_cast<int>(supportedSubtypeCount());
    *out_size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    LOG("-> S_OK (count=%d)\n", *out_count);
    return S_OK;
}

HRESULT
Softcam::GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc)
{
    if (!out_pmt || !out_scc)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (!m_valid)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= supportedSubtypeCount())
    {
        LOG("-> S_FALSE (invalid index %d)\n", index);
        return S_FALSE;
    }
    const SubtypeInfo& sub = supportedSubtypes()[index];
    AM_MEDIA_TYPE *mt = makeMediaType(m_width, m_height, m_framerate, sub);
    if (!mt)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }
    *out_pmt = mt;
    std::memset(out_scc, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    VIDEOINFOHEADER* format = (VIDEOINFOHEADER*)mt->pbFormat;
    VIDEO_STREAM_CONFIG_CAPS* scc = (VIDEO_STREAM_CONFIG_CAPS*)out_scc;
    scc->guid = FORMAT_VideoInfo;
    scc->InputSize = SIZE{m_width, m_height};
    scc->MinCroppingSize = SIZE{m_width, m_height};
    scc->MaxCroppingSize = SIZE{m_width, m_height};
    scc->CropGranularityX = 1;
    scc->CropGranularityY = 1;
    scc->CropAlignX = 1;
    scc->CropAlignY = 1;
    scc->MinOutputSize = SIZE{m_width, m_height};
    scc->MaxOutputSize = SIZE{m_width, m_height};
    scc->OutputGranularityX = 1;
    scc->OutputGranularityY = 1;
    scc->StretchTapsX = 0;
    scc->StretchTapsY = 0;
    scc->ShrinkTapsX = 0;
    scc->ShrinkTapsY = 0;
    scc->MinFrameInterval = format->AvgTimePerFrame;
    scc->MaxFrameInterval = format->AvgTimePerFrame;
    scc->MinBitsPerSecond = (LONG)format->dwBitRate;
    scc->MaxBitsPerSecond = (LONG)format->dwBitRate;
    LOG("-> S_OK (subtype=%ws)\n", sub.label);
    return S_OK;
}


FrameBuffer* Softcam::getFrameBuffer()
{
    if (!m_valid)
    {
        return nullptr;
    }

    CAutoLock lock(&m_critsec);
    if (!m_frame_buffer)
    {
        auto fb = FrameBuffer::open();
        if (fb &&
            fb.active() &&
            fb.width() == m_width &&
            fb.height() == m_height)
        {
            m_frame_buffer = fb;
        }
    }
    if (m_frame_buffer)
    {
        return &m_frame_buffer;
    }
    else
    {
        return nullptr;
    }
}

void
Softcam::releaseFrameBuffer()
{
    CAutoLock lock(&m_critsec);
    m_frame_buffer.release();
}


// -------------------------------------------------------------------------
// SoftcamStream
// -------------------------------------------------------------------------

SoftcamStream::SoftcamStream(HRESULT *phr,
                         Softcam *pParent,
                         LPCWSTR pPinName) :
    CSourceStream(NAME("DirectShow Softcam Stream"), phr, pParent, pPinName),
    m_valid(pParent->valid()),
    m_width(pParent->width()),
    m_height(pParent->height()),
    m_bpp(pParent->bpp()),
    m_subtype(findSubtype(MEDIASUBTYPE_ARGB32))
{
}


SoftcamStream::~SoftcamStream()
{
    LOG("===== logging finished =====\n");
    CLOSE_LOGFILE();
}


STDMETHODIMP SoftcamStream::NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv)
{
    if (riid == IID_IKsPropertySet )
    {
        LOG("(SoftcamStream) IKsPropertySet -> S_OK\n");
        return GetInterface(static_cast<IKsPropertySet*>(this), ppv);
    }
    else if(riid == IID_IAMStreamConfig)
    {
        LOG("(SoftcamStream) IAMStreamConfig -> S_OK\n");
        return GetInterface(static_cast<IAMStreamConfig*>(this), ppv);
    }
    else
    {
        auto result = CSourceStream::NonDelegatingQueryInterface(riid, ppv);
        LOG("(SoftcamStream) %s -> %s\n", IID_TO_STR(riid).c_str(), result ? "(ERROR)" : "S_OK");
        return result;
    }
}


namespace {

// Top-down BGRA32 -> bottom-up RGB32 / ARGB32 DIB rows.
//
// In shmem we store BGRA32 top-down. DIBs want bottom-up rows with the
// natural pixel byte order -- RGB32 and ARGB32 on little-endian both have
// the bytes in BGRA order on disk. So the only thing to do is flip rows.
// (No conversion needed: the wire format is already BGRA.)
//
// stride_bytes = ((w * 4) + 3) & ~3.
void copyBGRA_to_RGB32_dib(const uint8_t* src_topdown,
                           uint8_t*       dst_bottomup,
                           int w, int h)
{
    const std::uint32_t row_bytes = static_cast<std::uint32_t>(w) * 4u;
    const std::uint32_t stride    = (row_bytes + 3u) & ~3u;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* src_row = src_topdown + static_cast<std::size_t>(row_bytes) * y;
        uint8_t* dst_row = dst_bottomup + static_cast<std::size_t>(stride) * y;
        std::memcpy(dst_row, src_row, row_bytes);
        if (stride > row_bytes)
        {
            std::memset(dst_row + row_bytes, 0, stride - row_bytes);
        }
    }
}

// Fill a YUV/MJPG subtype buffer from a top-down BGRA32 scratch frame.
void fill_yuv_subtype(uint8_t* pData, int w, int h,
                      const SubtypeInfo& sub,
                      const uint8_t* bgra_topdown,
                      MjpegEncoder* mjpeg)
{
    if (sub.subtype == &MEDIASUBTYPE_YUY2)
    {
        softcam::convert_bgra_to_yuy2(bgra_topdown, pData,
                                      static_cast<std::size_t>(w) * h);
    }
    else if (sub.subtype == &MEDIASUBTYPE_UYVY)
    {
        softcam::convert_bgra_to_uyvy(bgra_topdown, pData,
                                      static_cast<std::size_t>(w) * h);
    }
    else if (sub.subtype == &MEDIASUBTYPE_YVYU)
    {
        softcam::convert_bgra_to_yvyu(bgra_topdown, pData,
                                      static_cast<std::size_t>(w) * h);
    }
    else if (sub.subtype == &MEDIASUBTYPE_IYUV)
    {
        // I420 layout: Y plane = w*h, U plane = (w/2)*(h/2), V plane.
        std::uint32_t y_stride = static_cast<std::uint32_t>(w);
        std::uint32_t u_stride = y_stride / 2u;
        std::uint8_t* y_plane = pData;
        std::uint8_t* u_plane = y_plane + y_stride * static_cast<std::uint32_t>(h);
        std::uint8_t* v_plane = u_plane + u_stride * (static_cast<std::uint32_t>(h) / 2u);
        softcam::convert_bgra_to_i420(bgra_topdown, y_plane, y_stride,
                                      u_plane, u_stride, v_plane, u_stride,
                                      static_cast<std::uint32_t>(w),
                                      static_cast<std::uint32_t>(h));
    }
    else if (sub.subtype == &MEDIASUBTYPE_YV12)
    {
        std::uint32_t y_stride = static_cast<std::uint32_t>(w);
        std::uint32_t u_stride = y_stride / 2u;
        std::uint8_t* y_plane = pData;
        std::uint8_t* v_plane = y_plane + y_stride * static_cast<std::uint32_t>(h);
        std::uint8_t* u_plane = v_plane + u_stride * (static_cast<std::uint32_t>(h) / 2u);
        softcam::convert_bgra_to_yv12(bgra_topdown, y_plane, y_stride,
                                      u_plane, u_stride, v_plane, u_stride,
                                      static_cast<std::uint32_t>(w),
                                      static_cast<std::uint32_t>(h));
    }
    else if (sub.subtype == &MEDIASUBTYPE_NV12)
    {
        std::uint32_t y_stride = static_cast<std::uint32_t>(w);
        std::uint32_t uv_stride = y_stride;
        std::uint8_t* y_plane = pData;
        std::uint8_t* uv_plane = y_plane + y_stride * static_cast<std::uint32_t>(h);
        softcam::convert_bgra_to_nv12(bgra_topdown, y_plane, y_stride,
                                      uv_plane, uv_stride,
                                      static_cast<std::uint32_t>(w),
                                      static_cast<std::uint32_t>(h));
    }
    else if (sub.subtype == &MEDIASUBTYPE_MJPG)
    {
        if (!mjpeg)
        {
            LOG("(SoftcamStream) MJPG encoder not initialized\n");
            return;
        }
        // The DShow allocator sizes the IMediaSample buffer from
        // biSizeImage (mjpgHintSize = w*h/2). For 1080p that's 1 MB which
        // is enough at quality 80. 4K scenes need a larger pool -- callers
        // can renegotiate via SetFormat after the initial connection.
        const std::size_t cap = static_cast<std::size_t>(w) * h / 2u;
        std::size_t written = mjpeg->encode(bgra_topdown, w, h, pData, cap);
        if (written == 0)
        {
            LOG("(SoftcamStream) MJPG encode failed, frame dropped\n");
        }
    }
    else
    {
        LOG("(SoftcamStream) unhandled subtype %ws in fill_yuv_subtype\n", sub.label);
    }
}

} //namespace


HRESULT SoftcamStream::FillBuffer(IMediaSample *pms)
{
    CheckPointer(pms,E_POINTER);

    BYTE *pData;
    pms->GetPointer(&pData);
    long lDataLen = pms->GetSize();
    ZeroMemory(pData, (std::size_t)lDataLen);
    {
        if (auto fb = getParent()->getFrameBuffer())
        {
            bool active = fb->waitForNewFrame(m_frame_counter);

            if (m_subtype && m_subtype->biCompression == BI_RGB)
            {
                // For ARGB32 / RGB32 the wire pixels are already BGRA32
                // top-down and the DIB wants BGRA bottom-up. transferToDIB
                // handles the row-flip in one shot.
                fb->transferToDIB(pData, &m_frame_counter);

                if (!active)
                {
                    const std::size_t size = (std::size_t)m_bpp * m_width * m_height;
                    if (!m_screenshot)
                    {
                        m_screenshot.reset(new uint8_t[size]);
                    }
                    for (std::size_t i = 0; i < size; i++)
                    {
                        pData[i] /= 4;
                    }
                    std::memcpy(m_screenshot.get(), pData, size);
                }
            }
            else
            {
                // YUV / MJPG path: pull a top-down BGRA scratch copy and
                // convert per-subtype. transferToDIB still increments
                // m_frame_counter via the FrameBuffer.
                const std::size_t bgra_bytes = (std::size_t)m_width * m_height * 4;
                if (!m_screenshot)
                {
                    m_screenshot.reset(new uint8_t[bgra_bytes]);
                }
                uint64_t ignored = 0;
                fb->transferToDIB(m_screenshot.get(), &ignored);

                std::unique_ptr<uint8_t[]> topdown(new uint8_t[bgra_bytes]);
                const std::uint32_t row_bytes = static_cast<std::uint32_t>(m_width) * 4u;
                for (int y = 0; y < m_height; ++y)
                {
                    std::memcpy(topdown.get() + (std::size_t)y * row_bytes,
                                m_screenshot.get() + (std::size_t)(m_height - 1 - y) * row_bytes,
                                row_bytes);
                }
                if (!active)
                {
                    // Use darkened screenshot as the source for YUV/MJPG.
                    for (std::size_t i = 0; i < bgra_bytes; i++)
                    {
                        topdown[i] /= 4;
                    }
                }
                if (m_subtype->subtype == &MEDIASUBTYPE_MJPG && !m_mjpeg)
                {
                    m_mjpeg.reset(new MjpegEncoder());
                }
                fill_yuv_subtype(pData, m_width, m_height, *m_subtype,
                                 topdown.get(), m_mjpeg.get());
            }
        }
        else
        {
            m_frame_counter = 0;
            Timer::sleep(0.100f);

            const std::size_t size = (std::size_t)m_bpp * m_width * m_height;
            if (!m_screenshot)
            {
                m_screenshot.reset(new uint8_t[size]);
            }
            std::memcpy(pData, m_screenshot.get(), size);
        }

        CAutoLock lock(&m_critsec);
        CRefTime start = m_sample_time;
        m_sample_time += (LONG)m_interval_time_msec;
        pms->SetTime((REFERENCE_TIME*)&start,(REFERENCE_TIME*)&m_sample_time);
    }
    pms->SetSyncPoint(TRUE);
    return NOERROR;
}


STDMETHODIMP SoftcamStream::Notify(IBaseFilter * pSender, Quality q)
{
    CAutoLock lock(&m_critsec);
    m_interval_time_msec = m_interval_time_msec * 1000 / (std::max)(q.Proportion, 1L);
    m_interval_time_msec = (std::min)((std::max)(m_interval_time_msec, 1L), 1000L);
    if (q.Late > 0) {
        m_sample_time += q.Late;
    }
    return NOERROR;
}


HRESULT SoftcamStream::GetMediaType(CMediaType *pmt)
{
    CheckPointer(pmt,E_POINTER);

    if (!m_valid)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }

    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
    if (pvi == nullptr)
    {
        LOG("-> E_OUTOFMEMORY\n");
        return E_OUTOFMEMORY;
    }

    const SubtypeInfo* sub = m_subtype ? m_subtype : findSubtype(MEDIASUBTYPE_ARGB32);
    fillMediaType(pmt, getParent()->width(), getParent()->height(),
                  getParent()->framerate(), *sub);

    LOG("-> NOERROR (subtype=%ws)\n", sub->label);
    return NOERROR;
}

HRESULT SoftcamStream::DecideBufferSize(IMemAllocator *pAlloc,
                                        ALLOCATOR_PROPERTIES *pProperties)
{
    CheckPointer(pAlloc,E_POINTER);
    CheckPointer(pProperties,E_POINTER);

    CAutoLock lock(m_pFilter->pStateLock());
    HRESULT hr = NOERROR;

    VIDEOINFO *pvi = (VIDEOINFO *)m_mt.Format();
    pProperties->cBuffers = 1;
    pProperties->cbBuffer = (long)pvi->bmiHeader.biSizeImage;

    ALLOCATOR_PROPERTIES actual;
    hr = pAlloc->SetProperties(pProperties, &actual);
    if (FAILED(hr))
    {
        LOG("-> (FAILED)\n");
        return hr;
    }
    if (actual.cbBuffer < pProperties->cbBuffer)
    {
        LOG("-> E_FAIL\n");
        return E_FAIL;
    }
    LOG("-> NOERROR\n");
    return NOERROR;
}

HRESULT SoftcamStream::OnThreadCreate()
{
    CAutoLock lock(&m_critsec);
    m_sample_time = 0;
    float framerate = getParent()->framerate();
    if (framerate <= 0.0f)
    {
        framerate = 60.0f;
    }
    framerate = (std::min)((std::max)(framerate, 1.0f), 1000.0f);
    m_interval_time_msec = (long)std::round(1000.0f / framerate);

    LOG("-> NOERROR\n");
    return NOERROR;
}

HRESULT SoftcamStream::Set(REFGUID guidPropSet, DWORD dwPropID,
                           LPVOID pInstanceData, DWORD cbInstanceData,
                           LPVOID pPropData, DWORD cbPropData)
{
    LOG("-> E_NOTIMPL\n");
    return E_NOTIMPL;
}

HRESULT SoftcamStream::Get(REFGUID guidPropSet, DWORD dwPropID,
                           LPVOID pInstanceData, DWORD cbInstanceData,
                           LPVOID pPropData, DWORD cbPropData, DWORD *pcbReturned)
{
    if (guidPropSet != AMPROPSETID_Pin)
    {
        LOG("-> E_PROP_SET_UNSUPPORTED\n");
        return E_PROP_SET_UNSUPPORTED;
    }
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)
    {
        LOG("-> E_PROP_ID_UNSUPPORTED\n");
        return E_PROP_ID_UNSUPPORTED;
    }
    if (pPropData == nullptr && pcbReturned == nullptr)
    {
        LOG("-> E_POINTER\n");
        return E_POINTER;
    }
    if (pcbReturned)
    {
        *pcbReturned = sizeof(GUID);
    }
    if (pPropData)
    {
        if (cbPropData < sizeof(GUID))
        {
            LOG("-> E_UNEXPECTED\n");
            return E_UNEXPECTED;
        }
        *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT SoftcamStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
                              DWORD *pTypeSupport)
{
    if (guidPropSet != AMPROPSETID_Pin)
    {
        LOG("-> E_PROP_SET_UNSUPPORTED\n");
        return E_PROP_SET_UNSUPPORTED;
    }
    if (dwPropID != AMPROPERTY_PIN_CATEGORY)
    {
        LOG("-> E_PROP_ID_UNSUPPORTED\n");
        return E_PROP_ID_UNSUPPORTED;
    }
    if (pTypeSupport)
    {
        *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    }
    LOG("-> S_OK\n");
    return S_OK;
}

HRESULT SoftcamStream::SetFormat(AM_MEDIA_TYPE *mt)
{
    if (!mt) return E_POINTER;
    const SubtypeInfo* sub = findSubtype(mt->subtype);
    if (!sub) return E_FAIL;
    HRESULT hr = getParent()->SetFormat(mt);
    if (SUCCEEDED(hr))
    {
        m_subtype = sub;
    }
    return hr;
}

HRESULT SoftcamStream::GetFormat(AM_MEDIA_TYPE **out_pmt)
{
    return getParent()->GetFormat(out_pmt);
}

HRESULT SoftcamStream::GetNumberOfCapabilities(int *out_count, int *out_size)
{
    return getParent()->GetNumberOfCapabilities(out_count, out_size);
}

HRESULT SoftcamStream::GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc)
{
    return getParent()->GetStreamCaps(index, out_pmt, out_scc);
}

Softcam* SoftcamStream::getParent()
{
    return static_cast<Softcam*>(m_pFilter);
}


} //namespace softcam