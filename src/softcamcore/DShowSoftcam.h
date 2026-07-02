#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <baseclasses/streams.h>
#include "FrameBuffer.h"


namespace softcam {


// Per-subtype media type descriptor. Each entry encodes everything needed
// to (a) build a VIDEOINFOHEADER for IAMStreamConfig::GetStreamCaps, (b)
// validate an inbound SetFormat, and (c) pick a FillBuffer conversion path.
struct SubtypeInfo
{
    const GUID*  subtype;          // MEDIASUBTYPE_*
    uint16_t     biBitCount;       // for BITMAPINFOHEADER
    uint32_t     biCompression;    // BI_RGB for RGB-family, FOURCC for YUV
    FOURCC       fourcc;           // for VIDEOINFOHEADER::bmiHeader (BI_RGB path uses 0)
    uint8_t      bpp;              // bytes per pixel for RGB paths; 0 for planar/encoded
    const wchar_t* label;          // human-readable, used for logs and PIN_CATEGORY
};

// Returns the static table of supported subtypes. Order is the
// enumeration order IAMStreamConfig::GetStreamCaps uses.
const SubtypeInfo*  supportedSubtypes();
std::size_t         supportedSubtypeCount();


// Looks up a SubtypeInfo by subtype GUID. Returns nullptr when unknown.
const SubtypeInfo*  findSubtype(const GUID& subtype);

// Compute the on-the-wire image size (bytes) for the given dimensions and
// subtype. For RGB-family this includes stride alignment (4-byte rows).
// MJPG returns 0 (variable-length payload).
std::uint32_t       calcDIBSize(int width, int height, const SubtypeInfo& sub);


class Softcam : public CSource, public IAMStreamConfig
{
public:
    static CUnknown* CreateInstance(
                    LPUNKNOWN   lpunk,
                    const GUID& clsid,
                    HRESULT*    phr);

    // IUnknown Methods
    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv) override;

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *mt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **out_pmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *out_count, int *out_size) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc) override;

    FrameBuffer*    getFrameBuffer();
    bool            valid() const { return m_valid; }
    int             width() const { return m_width; }
    int             height() const { return m_height; }
    float           framerate() const { return m_framerate; }
    int             bpp() const { return m_bpp; }
    void            releaseFrameBuffer();

private:
    CCritSec    m_critsec;
    FrameBuffer m_frame_buffer;
    const bool  m_valid;
    const int   m_width;
    const int   m_height;
    const int   m_bpp;
    const float m_framerate;

    Softcam(LPUNKNOWN lpunk, const GUID& clsid, HRESULT *phr);
};


class SoftcamStream : public CSourceStream, public IKsPropertySet, public IAMStreamConfig
{
 public:
    SoftcamStream(HRESULT *phr, Softcam *pParent, LPCWSTR pPinName);
    ~SoftcamStream();

    // IUnknown
    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, __deref_out void **ppv) override;

    // CBasePin
    STDMETHODIMP Notify(IBaseFilter * pSender, Quality q) override;

    // CBaseOutputPin
    HRESULT DecideBufferSize(IMemAllocator *pIMemAlloc,
                             ALLOCATOR_PROPERTIES *pProperties) override;

    // CSourceStream
    HRESULT FillBuffer(IMediaSample *pms) override;
    HRESULT GetMediaType(CMediaType *pMediaType) override;
    HRESULT OnThreadCreate(void) override;

    //  IKsPropertySet
    HRESULT STDMETHODCALLTYPE Set(REFGUID guidPropSet, DWORD dwPropID,
                                  LPVOID pInstanceData, DWORD cbInstanceData,
                                  LPVOID pPropData, DWORD cbPropData) override;
    HRESULT STDMETHODCALLTYPE Get(REFGUID guidPropSet, DWORD dwPropID,
                                  LPVOID pInstanceData, DWORD cbInstanceData,
                                  LPVOID pPropData, DWORD cbPropData, DWORD *pcbReturned) override;
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guidPropSet, DWORD dwPropID,
                                             DWORD *pTypeSupport) override;

    // IAMStreamConfig
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE *mt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE **out_pmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int *out_count, int *out_size) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE **out_pmt, BYTE *out_scc) override;

private:
    const bool          m_valid;
    const int           m_width;
    const int           m_height;
    const int           m_bpp;
    uint64_t            m_frame_counter = 0;
    std::unique_ptr<uint8_t[]>  m_screenshot;

    // Subtype currently negotiated with the consumer. Defaults to ARGB32.
    const SubtypeInfo*  m_subtype;

    // Lazily constructed on first MJPG fill. Encoding through WIC costs
    // ~10-30 ms per frame at 1080p; one encoder per streaming thread is
    // sufficient because DirectShow runs FillBuffer on a single thread.
    std::unique_ptr<class MjpegEncoder> m_mjpeg;

    CCritSec m_critsec;
    CRefTime m_sample_time;
    long m_interval_time_msec = 10;

    Softcam*        getParent();
};


} //namespace softcam