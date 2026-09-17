#ifndef RDPINPUTHANDLER_P_H
#define RDPINPUTHANDLER_P_H

#include "inputcontroller.h"

#include <QString>

#include <cstdint>

class RdpInputHandler final
{
public:
    explicit RdpInputHandler(InputController &controller);

    bool keyboardEvent(std::uint16_t flags,
                       std::uint8_t scanCode,
                       QString *errorMessage);
    bool unicodeKeyboardEvent(std::uint16_t flags,
                              std::uint16_t codeUnit,
                              QString *errorMessage);
    bool mouseEvent(std::uint16_t flags,
                    std::uint16_t x,
                    std::uint16_t y,
                    QString *errorMessage);
    bool relativeMouseEvent(std::uint16_t flags,
                            std::int16_t deltaX,
                            std::int16_t deltaY,
                            QString *errorMessage);
    bool extendedMouseEvent(std::uint16_t flags,
                            std::uint16_t x,
                            std::uint16_t y,
                            QString *errorMessage);

private:
    bool setButton(InputMouseButton button,
                   bool pressed,
                   QString *errorMessage);

    InputController &controller;
};

#endif // RDPINPUTHANDLER_P_H
