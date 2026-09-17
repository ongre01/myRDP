#ifndef INPUTCONTROLLER_H
#define INPUTCONTROLLER_H

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

enum class InputMouseButton
{
    Left,
    Right,
    Middle,
    X1,
    X2
};

enum class InputWheelAxis
{
    Vertical,
    Horizontal
};

class InputController
{
public:
    virtual ~InputController() = default;

    virtual bool sendScanCode(std::uint16_t scanCode,
                              bool released,
                              bool extended,
                              QString *errorMessage) = 0;
    virtual bool sendUnicodeCodeUnit(std::uint16_t codeUnit,
                                     bool released,
                                     QString *errorMessage) = 0;
    virtual bool movePointer(std::uint16_t x,
                             std::uint16_t y,
                             QString *errorMessage) = 0;
    virtual bool movePointerRelative(std::int16_t deltaX,
                                     std::int16_t deltaY,
                                     QString *errorMessage) = 0;
    virtual bool setMouseButton(InputMouseButton button,
                                bool pressed,
                                QString *errorMessage) = 0;
    virtual bool scroll(InputWheelAxis axis,
                        std::int16_t delta,
                        QString *errorMessage) = 0;
};

using InputControllerFactory = std::function<std::unique_ptr<InputController>()>;

std::unique_ptr<InputController> createInputController();

#endif // INPUTCONTROLLER_H
