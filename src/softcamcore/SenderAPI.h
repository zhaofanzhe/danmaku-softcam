#pragma once


namespace softcam {
namespace sender {

using CameraHandle = void*;

// Pixel format of the image data passed to SendFrame / SendFrameRGBA.
// Matches the values stored in FrameBuffer::Header::m_image_format so a
// `format` value can be passed directly through.
enum ImageFormat : int
{
    FORMAT_RGB24  = 0,
    FORMAT_RGBA32 = 1,
};

CameraHandle    CreateCamera(
                    int     width,
                    int     height,
                    float   framerate = 60.0f,
                    int     format    = FORMAT_RGB24);
void            DeleteCamera(CameraHandle camera);
void            SendFrame(CameraHandle camera, const void* image_bits);
void            SendFrameRGBA(CameraHandle camera, const void* rgba_bits);
bool            WaitForConnection(CameraHandle camera, float timeout = 0.0f);
bool            IsConnected(CameraHandle camera);

} //namespace sender
} //namespace softcam
