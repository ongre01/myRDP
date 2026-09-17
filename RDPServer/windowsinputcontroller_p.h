#ifndef WINDOWSINPUTCONTROLLER_P_H
#define WINDOWSINPUTCONTROLLER_P_H

#include "inputcontroller.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <qt_windows.h>

#include <functional>

class WindowsInputController final : public InputController
{
public:
    using InputSender = std::function<UINT(UINT, const INPUT *, int)>;
    using SystemMetricReader = std::function<int(int)>;

    explicit WindowsInputController(InputSender inputSender = {},
                                    SystemMetricReader systemMetricReader = {});

    bool sendScanCode(std::uint16_t scanCode,
                      bool released,
                      bool extended,
                      QString *errorMessage) override;
    bool sendUnicodeCodeUnit(std::uint16_t codeUnit,
                             bool released,
                             QString *errorMessage) override;
    bool movePointer(std::uint16_t x,
                     std::uint16_t y,
                     QString *errorMessage) override;
    bool movePointerRelative(std::int16_t deltaX,
                             std::int16_t deltaY,
                             QString *errorMessage) override;
    bool setMouseButton(InputMouseButton button,
                        bool pressed,
                        QString *errorMessage) override;
    bool scroll(InputWheelAxis axis,
                std::int16_t delta,
                QString *errorMessage) override;

private:
    bool inject(const INPUT &input,
                const QString &operation,
                QString *errorMessage) const;

    InputSender inputSender;
    SystemMetricReader systemMetricReader;
};

#endif // WINDOWSINPUTCONTROLLER_P_H
