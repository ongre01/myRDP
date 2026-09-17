#include "desktopcapture.h"

#include <QtGlobal>

#include <limits>

#if defined(Q_OS_WIN)
#include "windowsdesktopcapture_p.h"
#endif

bool DesktopSize::isValid() const
{
    return width > 0 && height > 0;
}

bool operator==(const DesktopSize &left, const DesktopSize &right)
{
    return left.width == right.width && left.height == right.height;
}

bool operator!=(const DesktopSize &left, const DesktopSize &right)
{
    return !(left == right);
}

bool DesktopFrame::isValid() const
{
    if (!desktopSize.isValid() || width == 0 || height == 0
        || x > desktopSize.width || y > desktopSize.height
        || width > desktopSize.width - x || height > desktopSize.height - y) {
        return false;
    }

    const std::uint64_t minimumStride = static_cast<std::uint64_t>(width)
                                        * desktopCaptureBytesPerPixel;
    if (minimumStride > (std::numeric_limits<std::uint32_t>::max)()
        || stride < minimumStride) {
        return false;
    }

    const std::uint64_t requiredBytes = static_cast<std::uint64_t>(stride) * height;
    return requiredBytes <= pixels.size();
}

std::unique_ptr<DesktopCapture> createDesktopCapture()
{
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsDesktopCapture>();
#else
    return nullptr;
#endif
}
