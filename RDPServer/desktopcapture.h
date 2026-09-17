#ifndef DESKTOPCAPTURE_H
#define DESKTOPCAPTURE_H

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

constexpr std::uint32_t desktopCaptureBytesPerPixel = 4;

struct DesktopSize
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool isValid() const;
};

bool operator==(const DesktopSize &left, const DesktopSize &right);
bool operator!=(const DesktopSize &left, const DesktopSize &right);

struct DesktopFrame
{
    DesktopSize desktopSize;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> pixels;
    bool desktopSizeChanged = false;

    bool isValid() const;
};

enum class DesktopCaptureStatus
{
    FrameReady,
    NoChanges,
    Error
};

struct DesktopCaptureResult
{
    DesktopCaptureStatus status = DesktopCaptureStatus::Error;
    DesktopFrame frame;
    QString errorMessage;
};

class DesktopCapture
{
public:
    virtual ~DesktopCapture() = default;

    virtual DesktopSize desktopSize(QString *errorMessage) = 0;
    virtual DesktopCaptureResult capture(bool forceFullFrame) = 0;
};

using DesktopCaptureFactory = std::function<std::unique_ptr<DesktopCapture>()>;

std::unique_ptr<DesktopCapture> createDesktopCapture();

#endif // DESKTOPCAPTURE_H
