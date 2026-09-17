#ifndef RDPINPUT_H
#define RDPINPUT_H

#include <QPoint>
#include <QtGlobal>

struct RdpKeyboardInput
{
    enum class Kind
    {
        ScanCode,
        Pause
    };

    Kind kind = Kind::ScanCode;
    quint32 scanCode = 0;
    bool pressed = false;
    bool repeat = false;
};

struct RdpPointerInput
{
    enum class Kind
    {
        Move,
        LeftButton,
        RightButton,
        VerticalWheel
    };

    Kind kind = Kind::Move;
    QPoint position;
    bool pressed = false;
    int wheelDelta = 0;
};

#endif // RDPINPUT_H
