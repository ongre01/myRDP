#ifndef WINDOWSDESKTOPCAPTURE_P_H
#define WINDOWSDESKTOPCAPTURE_P_H

#include "desktopcapture.h"
#include "desktopframebuffer_p.h"

#include <qt_windows.h>

class WindowsDesktopCapture final : public DesktopCapture
{
public:
    WindowsDesktopCapture() = default;
    ~WindowsDesktopCapture() override;

    DesktopSize desktopSize(QString *errorMessage) override;
    DesktopCaptureResult capture(bool forceFullFrame) override;

private:
    struct DesktopGeometry
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    bool queryGeometry(DesktopGeometry *geometry, QString *errorMessage) const;
    bool ensureCaptureSurface(const DesktopGeometry &geometry,
                              bool *surfaceChanged,
                              QString *errorMessage);
    void releaseCaptureSurface();

    HDC desktopDc = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void *bitmapBits = nullptr;
    DesktopGeometry currentGeometry;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> capturePixels;
    DesktopFrameBuffer frameBuffer;
};

#endif // WINDOWSDESKTOPCAPTURE_P_H
