#include "windowsinputcontroller_p.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {
QString windowsErrorMessage(const QString &operation, DWORD errorCode)
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

LONG normalizedPointerCoordinate(std::uint16_t coordinate, int extent)
{
    if (extent <= 1) {
        return 0;
    }

    const std::uint64_t lastPixel = static_cast<std::uint64_t>(extent - 1);
    const std::uint64_t clamped = (std::min)(static_cast<std::uint64_t>(coordinate),
                                             lastPixel);
    return static_cast<LONG>((clamped * 65535ULL + lastPixel / 2ULL) / lastPixel);
}
} // namespace

WindowsInputController::WindowsInputController(InputSender inputSender,
                                               SystemMetricReader systemMetricReader)
    : inputSender(std::move(inputSender))
    , systemMetricReader(std::move(systemMetricReader))
{
    if (!this->inputSender) {
        this->inputSender = [](UINT count, const INPUT *inputs, int size) {
            return SendInput(count, const_cast<INPUT *>(inputs), size);
        };
    }
    if (!this->systemMetricReader) {
        this->systemMetricReader = [](int index) { return GetSystemMetrics(index); };
    }
}

bool WindowsInputController::sendScanCode(std::uint16_t scanCode,
                                          bool released,
                                          bool extended,
                                          QString *errorMessage)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (released) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    if (extended) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    return inject(input, QStringLiteral("Failed to inject keyboard input"), errorMessage);
}

bool WindowsInputController::sendUnicodeCodeUnit(std::uint16_t codeUnit,
                                                 bool released,
                                                 QString *errorMessage)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = codeUnit;
    input.ki.dwFlags = KEYEVENTF_UNICODE;
    if (released) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return inject(input, QStringLiteral("Failed to inject Unicode keyboard input"), errorMessage);
}

bool WindowsInputController::movePointer(std::uint16_t x,
                                         std::uint16_t y,
                                         QString *errorMessage)
{
    const int width = systemMetricReader(SM_CXVIRTUALSCREEN);
    const int height = systemMetricReader(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The Windows virtual desktop has an invalid size.");
        }
        return false;
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = normalizedPointerCoordinate(x, width);
    input.mi.dy = normalizedPointerCoordinate(y, height);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return inject(input, QStringLiteral("Failed to inject mouse movement"), errorMessage);
}

bool WindowsInputController::movePointerRelative(std::int16_t deltaX,
                                                 std::int16_t deltaY,
                                                 QString *errorMessage)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = deltaX;
    input.mi.dy = deltaY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return inject(input, QStringLiteral("Failed to inject relative mouse movement"), errorMessage);
}

bool WindowsInputController::setMouseButton(InputMouseButton button,
                                            bool pressed,
                                            QString *errorMessage)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
    case InputMouseButton::Left:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case InputMouseButton::Right:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case InputMouseButton::Middle:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case InputMouseButton::X1:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1;
        break;
    case InputMouseButton::X2:
        input.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2;
        break;
    }

    return inject(input, QStringLiteral("Failed to inject mouse button input"), errorMessage);
}

bool WindowsInputController::scroll(InputWheelAxis axis,
                                    std::int16_t delta,
                                    QString *errorMessage)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = axis == InputWheelAxis::Vertical ? MOUSEEVENTF_WHEEL
                                                        : MOUSEEVENTF_HWHEEL;
    input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(delta));
    return inject(input, QStringLiteral("Failed to inject mouse wheel input"), errorMessage);
}

bool WindowsInputController::inject(const INPUT &input,
                                    const QString &operation,
                                    QString *errorMessage) const
{
    SetLastError(ERROR_SUCCESS);
    if (inputSender(1, &input, sizeof(INPUT)) == 1) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage = windowsErrorMessage(operation, GetLastError());
    }
    return false;
}
