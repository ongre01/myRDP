#ifndef DESKTOPFRAMEBUFFER_P_H
#define DESKTOPFRAMEBUFFER_P_H

#include "desktopcapture.h"

class DesktopFrameBuffer final
{
public:
    DesktopCaptureResult update(const DesktopSize &size,
                                std::uint32_t stride,
                                const std::vector<std::uint8_t> &pixels,
                                bool forceFullFrame);

private:
    DesktopSize previousSize;
    std::uint32_t previousStride = 0;
    std::vector<std::uint8_t> previousPixels;
};

#endif // DESKTOPFRAMEBUFFER_P_H
