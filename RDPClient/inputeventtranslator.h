#ifndef INPUTEVENTTRANSLATOR_H
#define INPUTEVENTTRANSLATOR_H

#include "rdpinput.h"

#include <QPointF>
#include <QRectF>
#include <QSize>

#include <optional>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

class InputEventTranslator
{
public:
    static std::optional<RdpKeyboardInput> keyboardInput(const QKeyEvent &event);
    static std::optional<RdpPointerInput> mouseMoveInput(const QMouseEvent &event,
                                                         const QRectF &desktopTarget,
                                                         const QSize &desktopSize,
                                                         bool clampToDesktop = false);
    static std::optional<RdpPointerInput> mouseButtonInput(const QMouseEvent &event,
                                                           const QRectF &desktopTarget,
                                                           const QSize &desktopSize,
                                                           bool clampToDesktop = false);
    static std::optional<RdpPointerInput> wheelInput(const QWheelEvent &event,
                                                     const QRectF &desktopTarget,
                                                     const QSize &desktopSize);
    static std::optional<QPoint> remotePosition(const QPointF &widgetPosition,
                                                const QRectF &desktopTarget,
                                                const QSize &desktopSize,
                                                bool clampToDesktop = false);
};

#endif // INPUTEVENTTRANSLATOR_H
