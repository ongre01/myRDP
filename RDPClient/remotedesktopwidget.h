#ifndef REMOTEDESKTOPWIDGET_H
#define REMOTEDESKTOPWIDGET_H

#include "rdpinput.h"

#include <QFrame>
#include <QImage>
#include <QSet>

#include <functional>

class RemoteDesktopWidget : public QFrame
{
    Q_OBJECT

public:
    using KeyboardInputHandler = std::function<bool(const RdpKeyboardInput &)>;
    using PointerInputHandler = std::function<bool(const RdpPointerInput &)>;

    explicit RemoteDesktopWidget(QWidget *parent = nullptr);

    void setPlaceholderMessage(const QString &message);
    void updateDesktopRegion(const QSize &desktopSize,
                             const QRect &dirtyRect,
                             const QImage &regionImage);
    void clearDesktop();
    void setInputHandlers(KeyboardInputHandler keyboardHandler,
                          PointerInputHandler pointerHandler);
    void setInputEnabled(bool enabled);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    QRectF desktopTargetRect() const;
    void updateDirtyRegion(const QRect &dirtyRect);
    void handleKeyboardEvent(QKeyEvent *event);
    void handleMouseButtonEvent(QMouseEvent *event);
    bool dispatchKeyboardInput(const RdpKeyboardInput &input);
    bool dispatchPointerInput(const RdpPointerInput &input);
    void releasePressedInputs();
    void clearInputState();

    QImage desktopImage;
    QString placeholderMessage;
    KeyboardInputHandler keyboardInputHandler;
    PointerInputHandler pointerInputHandler;
    QSet<quint32> pressedScanCodes;
    Qt::MouseButtons forwardedMouseButtons;
    QPoint lastRemotePosition;
    bool inputEnabled = false;
};

#endif // REMOTEDESKTOPWIDGET_H
