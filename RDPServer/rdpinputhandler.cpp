#include "rdpinputhandler_p.h"

#include <freerdp/input.h>

namespace {
void setFallbackError(QString *errorMessage, const QString &fallback)
{
    if (errorMessage && errorMessage->isEmpty()) {
        *errorMessage = fallback;
    }
}
} // namespace

RdpInputHandler::RdpInputHandler(InputController &controller)
    : controller(controller)
{
}

bool RdpInputHandler::keyboardEvent(std::uint16_t flags,
                                    std::uint8_t scanCode,
                                    QString *errorMessage)
{
    const bool released = (flags & KBD_FLAGS_RELEASE) != 0;
    const bool extended = (flags & KBD_FLAGS_EXTENDED) != 0;
    if (controller.sendScanCode(scanCode, released, extended, errorMessage)) {
        return true;
    }
    setFallbackError(errorMessage, QStringLiteral("Failed to apply remote keyboard input."));
    return false;
}

bool RdpInputHandler::unicodeKeyboardEvent(std::uint16_t flags,
                                           std::uint16_t codeUnit,
                                           QString *errorMessage)
{
    const bool released = (flags & KBD_FLAGS_RELEASE) != 0;
    if (controller.sendUnicodeCodeUnit(codeUnit, released, errorMessage)) {
        return true;
    }
    setFallbackError(errorMessage, QStringLiteral("Failed to apply remote Unicode keyboard input."));
    return false;
}

bool RdpInputHandler::mouseEvent(std::uint16_t flags,
                                 std::uint16_t x,
                                 std::uint16_t y,
                                 QString *errorMessage)
{
    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) {
        std::int16_t delta = static_cast<std::int16_t>(flags & 0x00FF);
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) {
            delta = static_cast<std::int16_t>(delta - 0x0100);
        }
        const InputWheelAxis axis = (flags & PTR_FLAGS_WHEEL) != 0
                                        ? InputWheelAxis::Vertical
                                        : InputWheelAxis::Horizontal;
        if (controller.scroll(axis, delta, errorMessage)) {
            return true;
        }
        setFallbackError(errorMessage, QStringLiteral("Failed to apply remote mouse wheel input."));
        return false;
    }

    if ((flags & PTR_FLAGS_MOVE) != 0
        && !controller.movePointer(x, y, errorMessage)) {
        setFallbackError(errorMessage, QStringLiteral("Failed to apply remote mouse movement."));
        return false;
    }

    const bool pressed = (flags & PTR_FLAGS_DOWN) != 0;
    if ((flags & PTR_FLAGS_BUTTON1) != 0
        && !setButton(InputMouseButton::Left, pressed, errorMessage)) {
        return false;
    }
    if ((flags & PTR_FLAGS_BUTTON2) != 0
        && !setButton(InputMouseButton::Right, pressed, errorMessage)) {
        return false;
    }
    if ((flags & PTR_FLAGS_BUTTON3) != 0
        && !setButton(InputMouseButton::Middle, pressed, errorMessage)) {
        return false;
    }
    return true;
}

bool RdpInputHandler::relativeMouseEvent(std::uint16_t flags,
                                         std::int16_t deltaX,
                                         std::int16_t deltaY,
                                         QString *errorMessage)
{
    if (!controller.movePointerRelative(deltaX, deltaY, errorMessage)) {
        setFallbackError(errorMessage, QStringLiteral("Failed to apply remote relative mouse movement."));
        return false;
    }

    const bool pressed = (flags & PTR_FLAGS_DOWN) != 0;
    if ((flags & PTR_FLAGS_BUTTON1) != 0
        && !setButton(InputMouseButton::Left, pressed, errorMessage)) {
        return false;
    }
    if ((flags & PTR_FLAGS_BUTTON2) != 0
        && !setButton(InputMouseButton::Right, pressed, errorMessage)) {
        return false;
    }
    if ((flags & PTR_FLAGS_BUTTON3) != 0
        && !setButton(InputMouseButton::Middle, pressed, errorMessage)) {
        return false;
    }
    return true;
}

bool RdpInputHandler::extendedMouseEvent(std::uint16_t flags,
                                         std::uint16_t x,
                                         std::uint16_t y,
                                         QString *errorMessage)
{
    if ((flags & PTR_FLAGS_MOVE) != 0
        && !controller.movePointer(x, y, errorMessage)) {
        setFallbackError(errorMessage, QStringLiteral("Failed to apply remote mouse movement."));
        return false;
    }

    const bool pressed = (flags & PTR_XFLAGS_DOWN) != 0;
    if ((flags & PTR_XFLAGS_BUTTON1) != 0
        && !setButton(InputMouseButton::X1, pressed, errorMessage)) {
        return false;
    }
    if ((flags & PTR_XFLAGS_BUTTON2) != 0
        && !setButton(InputMouseButton::X2, pressed, errorMessage)) {
        return false;
    }
    return true;
}

bool RdpInputHandler::setButton(InputMouseButton button,
                                bool pressed,
                                QString *errorMessage)
{
    if (controller.setMouseButton(button, pressed, errorMessage)) {
        return true;
    }
    setFallbackError(errorMessage, QStringLiteral("Failed to apply remote mouse button input."));
    return false;
}
