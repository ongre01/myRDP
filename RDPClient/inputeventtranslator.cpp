#include "inputeventtranslator.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <QtMath>

#include <freerdp/scancode.h>
#include <winpr/input.h>

namespace {

std::optional<quint32> rdpScanCode(const QKeyEvent &event)
{
#if defined(Q_OS_WIN)
    const quint32 nativeScanCode = event.nativeScanCode();
    if (nativeScanCode == 0) {
        return std::nullopt;
    }

    // Qt represents the Windows E0 extended-key prefix as 0xE000.
    const bool extended = (nativeScanCode & 0xE000u) == 0xE000u;
    const quint32 scanCode = nativeScanCode & 0xFFu;
    if (scanCode == 0) {
        return std::nullopt;
    }
    return static_cast<quint32>(MAKE_RDP_SCANCODE(scanCode, extended));
#elif defined(Q_OS_MACOS)
    const DWORD virtualKey = GetVirtualKeyCodeFromKeycode(
        static_cast<DWORD>(event.nativeVirtualKey()), WINPR_KEYCODE_TYPE_APPLE);
    const DWORD scanCode = GetVirtualScanCodeFromVirtualKeyCode(
        virtualKey, WINPR_KBD_TYPE_IBM_ENHANCED);
    return scanCode == RDP_SCANCODE_UNKNOWN
               ? std::nullopt
               : std::optional<quint32>(static_cast<quint32>(scanCode));
#else
    const DWORD virtualKey = GetVirtualKeyCodeFromKeycode(
        static_cast<DWORD>(event.nativeScanCode()), WINPR_KEYCODE_TYPE_EVDEV);
    const DWORD scanCode = GetVirtualScanCodeFromVirtualKeyCode(
        virtualKey, WINPR_KBD_TYPE_IBM_ENHANCED);
    return scanCode == RDP_SCANCODE_UNKNOWN
               ? std::nullopt
               : std::optional<quint32>(static_cast<quint32>(scanCode));
#endif
}

} // namespace

std::optional<RdpKeyboardInput> InputEventTranslator::keyboardInput(const QKeyEvent &event)
{
    const bool pressed = event.type() == QEvent::KeyPress;
    if (!pressed && event.type() != QEvent::KeyRelease) {
        return std::nullopt;
    }

    // Qt synthesizes an auto-repeat release immediately before the repeated press.
    // RDP only needs the repeated press plus the final physical release.
    if (!pressed && event.isAutoRepeat()) {
        return std::nullopt;
    }

    if (event.key() == Qt::Key_Pause) {
        if (!pressed) {
            return std::nullopt;
        }

        RdpKeyboardInput input;
        input.kind = RdpKeyboardInput::Kind::Pause;
        input.pressed = true;
        return input;
    }

    const std::optional<quint32> scanCode = rdpScanCode(event);
    if (!scanCode) {
        return std::nullopt;
    }

    RdpKeyboardInput input;
    input.scanCode = *scanCode;
    input.pressed = pressed;
    input.repeat = pressed && event.isAutoRepeat();
    return input;
}

std::optional<RdpPointerInput> InputEventTranslator::mouseMoveInput(
    const QMouseEvent &event,
    const QRectF &desktopTarget,
    const QSize &desktopSize,
    bool clampToDesktop)
{
    const std::optional<QPoint> position = remotePosition(event.position(),
                                                          desktopTarget,
                                                          desktopSize,
                                                          clampToDesktop);
    if (!position) {
        return std::nullopt;
    }

    RdpPointerInput input;
    input.kind = RdpPointerInput::Kind::Move;
    input.position = *position;
    return input;
}

std::optional<RdpPointerInput> InputEventTranslator::mouseButtonInput(
    const QMouseEvent &event,
    const QRectF &desktopTarget,
    const QSize &desktopSize,
    bool clampToDesktop)
{
    RdpPointerInput::Kind kind;
    switch (event.button()) {
    case Qt::LeftButton:
        kind = RdpPointerInput::Kind::LeftButton;
        break;
    case Qt::RightButton:
        kind = RdpPointerInput::Kind::RightButton;
        break;
    default:
        return std::nullopt;
    }

    const std::optional<QPoint> position = remotePosition(event.position(),
                                                          desktopTarget,
                                                          desktopSize,
                                                          clampToDesktop);
    if (!position) {
        return std::nullopt;
    }

    RdpPointerInput input;
    input.kind = kind;
    input.position = *position;
    input.pressed = event.type() == QEvent::MouseButtonPress;
    return input;
}

std::optional<RdpPointerInput> InputEventTranslator::wheelInput(
    const QWheelEvent &event,
    const QRectF &desktopTarget,
    const QSize &desktopSize)
{
    const std::optional<QPoint> position = remotePosition(event.position(),
                                                          desktopTarget,
                                                          desktopSize);
    if (!position) {
        return std::nullopt;
    }

    int delta = event.angleDelta().y();
    if (delta == 0 && event.pixelDelta().y() != 0) {
        delta = event.pixelDelta().y() > 0 ? 120 : -120;
    }
    if (delta == 0) {
        return std::nullopt;
    }

    RdpPointerInput input;
    input.kind = RdpPointerInput::Kind::VerticalWheel;
    input.position = *position;
    input.wheelDelta = delta;
    return input;
}

std::optional<QPoint> InputEventTranslator::remotePosition(const QPointF &widgetPosition,
                                                            const QRectF &desktopTarget,
                                                            const QSize &desktopSize,
                                                            bool clampToDesktop)
{
    if (!desktopSize.isValid() || desktopTarget.isEmpty()) {
        return std::nullopt;
    }

    if (!clampToDesktop && !desktopTarget.contains(widgetPosition)) {
        return std::nullopt;
    }

    const qreal x = qBound(desktopTarget.left(), widgetPosition.x(), desktopTarget.right());
    const qreal y = qBound(desktopTarget.top(), widgetPosition.y(), desktopTarget.bottom());
    const qreal normalizedX = (x - desktopTarget.left()) / desktopTarget.width();
    const qreal normalizedY = (y - desktopTarget.top()) / desktopTarget.height();

    const int remoteX = qBound(0,
                               qFloor(normalizedX * desktopSize.width()),
                               desktopSize.width() - 1);
    const int remoteY = qBound(0,
                               qFloor(normalizedY * desktopSize.height()),
                               desktopSize.height() - 1);
    return QPoint(remoteX, remoteY);
}
