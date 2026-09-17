#include "windowsdesktopcapture_p.h"

#include <cstring>
#include <limits>

namespace {
QString windowsErrorMessage(const QString &operation, DWORD errorCode = GetLastError())
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                            | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        errorCode,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<wchar_t *>(&buffer),
                                        0,
                                        nullptr);
    QString detail;
    if (length > 0 && buffer) {
        detail = QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed();
    }
    if (buffer) {
        LocalFree(buffer);
    }

    return detail.isEmpty()
               ? QStringLiteral("%1 (Windows error %2).").arg(operation).arg(errorCode)
               : QStringLiteral("%1: %2 (Windows error %3).")
                     .arg(operation, detail)
                     .arg(errorCode);
}

DesktopCaptureResult captureError(const QString &message)
{
    DesktopCaptureResult result;
    result.status = DesktopCaptureStatus::Error;
    result.errorMessage = message;
    return result;
}
} // namespace

WindowsDesktopCapture::~WindowsDesktopCapture()
{
    releaseCaptureSurface();
}

DesktopSize WindowsDesktopCapture::desktopSize(QString *errorMessage)
{
    DesktopGeometry geometry;
    if (!queryGeometry(&geometry, errorMessage)) {
        return {};
    }
    return {static_cast<std::uint32_t>(geometry.width),
            static_cast<std::uint32_t>(geometry.height)};
}

DesktopCaptureResult WindowsDesktopCapture::capture(bool forceFullFrame)
{
    DesktopGeometry geometry;
    QString errorMessage;
    if (!queryGeometry(&geometry, &errorMessage)) {
        return captureError(errorMessage);
    }

    bool surfaceChanged = false;
    if (!ensureCaptureSurface(geometry, &surfaceChanged, &errorMessage)) {
        return captureError(errorMessage);
    }

    SetLastError(ERROR_SUCCESS);
    if (!BitBlt(memoryDc,
                0,
                0,
                geometry.width,
                geometry.height,
                desktopDc,
                geometry.x,
                geometry.y,
                SRCCOPY | CAPTUREBLT)) {
        return captureError(windowsErrorMessage(QStringLiteral("Desktop capture failed")));
    }

    const std::uint64_t byteCount = static_cast<std::uint64_t>(stride) * geometry.height;
    if (!bitmapBits || byteCount > (std::numeric_limits<std::size_t>::max)()) {
        return captureError(QStringLiteral("The Windows desktop capture buffer is invalid."));
    }

    try {
        capturePixels.resize(static_cast<std::size_t>(byteCount));
    } catch (...) {
        return captureError(QStringLiteral("Failed to allocate the Windows desktop capture buffer."));
    }
    std::memcpy(capturePixels.data(), bitmapBits, capturePixels.size());

    const DesktopSize size = {static_cast<std::uint32_t>(geometry.width),
                              static_cast<std::uint32_t>(geometry.height)};
    return frameBuffer.update(size,
                              stride,
                              capturePixels,
                              forceFullFrame || surfaceChanged);
}

bool WindowsDesktopCapture::queryGeometry(DesktopGeometry *geometry,
                                          QString *errorMessage) const
{
    if (!geometry) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The desktop geometry output is missing.");
        }
        return false;
    }

    DesktopGeometry queried;
    queried.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    queried.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    queried.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    queried.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (queried.width <= 0 || queried.height <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Windows returned an invalid desktop resolution (%1x%2).")
                                .arg(queried.width)
                                .arg(queried.height);
        }
        return false;
    }

    *geometry = queried;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool WindowsDesktopCapture::ensureCaptureSurface(const DesktopGeometry &geometry,
                                                 bool *surfaceChanged,
                                                 QString *errorMessage)
{
    const bool geometryChanged = !desktopDc || geometry.x != currentGeometry.x
                                 || geometry.y != currentGeometry.y
                                 || geometry.width != currentGeometry.width
                                 || geometry.height != currentGeometry.height;
    if (surfaceChanged) {
        *surfaceChanged = geometryChanged;
    }
    if (!geometryChanged) {
        return true;
    }

    releaseCaptureSurface();
    desktopDc = GetDC(nullptr);
    if (!desktopDc) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Failed to open the desktop device context"));
        }
        return false;
    }

    memoryDc = CreateCompatibleDC(desktopDc);
    if (!memoryDc) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Failed to create the desktop capture device context"));
        }
        releaseCaptureSurface();
        return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = geometry.width;
    bitmapInfo.bmiHeader.biHeight = -geometry.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    bitmap = CreateDIBSection(desktopDc,
                              &bitmapInfo,
                              DIB_RGB_COLORS,
                              &bitmapBits,
                              nullptr,
                              0);
    if (!bitmap || !bitmapBits) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Failed to create the desktop capture bitmap"));
        }
        releaseCaptureSurface();
        return false;
    }

    previousBitmap = SelectObject(memoryDc, bitmap);
    if (!previousBitmap || previousBitmap == HGDI_ERROR) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("Failed to select the desktop capture bitmap"));
        }
        previousBitmap = nullptr;
        releaseCaptureSurface();
        return false;
    }

    const std::uint64_t calculatedStride = static_cast<std::uint64_t>(geometry.width)
                                           * desktopCaptureBytesPerPixel;
    if (calculatedStride > (std::numeric_limits<std::uint32_t>::max)()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The Windows desktop is too wide to capture.");
        }
        releaseCaptureSurface();
        return false;
    }

    currentGeometry = geometry;
    stride = static_cast<std::uint32_t>(calculatedStride);
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

void WindowsDesktopCapture::releaseCaptureSurface()
{
    if (memoryDc && previousBitmap) {
        SelectObject(memoryDc, previousBitmap);
    }
    previousBitmap = nullptr;
    if (bitmap) {
        DeleteObject(bitmap);
    }
    bitmap = nullptr;
    bitmapBits = nullptr;
    if (memoryDc) {
        DeleteDC(memoryDc);
    }
    memoryDc = nullptr;
    if (desktopDc) {
        ReleaseDC(nullptr, desktopDc);
    }
    desktopDc = nullptr;
    currentGeometry = {};
    stride = 0;
}
