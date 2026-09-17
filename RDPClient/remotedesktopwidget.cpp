#include "remotedesktopwidget.h"

#include "inputeventtranslator.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QWheelEvent>

#include <utility>

RemoteDesktopWidget::RemoteDesktopWidget(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void RemoteDesktopWidget::setPlaceholderMessage(const QString &message)
{
    if (placeholderMessage == message) {
        return;
    }

    placeholderMessage = message;
    if (desktopImage.isNull()) {
        update();
    }
}

void RemoteDesktopWidget::updateDesktopRegion(const QSize &desktopSize,
                                              const QRect &dirtyRect,
                                              const QImage &regionImage)
{
    if (!desktopSize.isValid() || dirtyRect.isEmpty() || regionImage.isNull()) {
        return;
    }

    const QRect desktopBounds(QPoint(0, 0), desktopSize);
    const QRect clippedRect = dirtyRect.intersected(desktopBounds);
    if (clippedRect.isEmpty()) {
        return;
    }

    const bool sizeChanged = desktopImage.size() != desktopSize;
    if (sizeChanged) {
        desktopImage = QImage(desktopSize, QImage::Format_RGB32);
        desktopImage.fill(Qt::black);
    }

    const QPoint sourceOffset = clippedRect.topLeft() - dirtyRect.topLeft();
    QPainter imagePainter(&desktopImage);
    imagePainter.setCompositionMode(QPainter::CompositionMode_Source);
    imagePainter.drawImage(clippedRect.topLeft(), regionImage, QRect(sourceOffset, clippedRect.size()));
    imagePainter.end();

    if (sizeChanged) {
        update();
    } else {
        updateDirtyRegion(clippedRect);
    }
}

void RemoteDesktopWidget::clearDesktop()
{
    if (desktopImage.isNull()) {
        return;
    }

    desktopImage = QImage();
    update();
}

void RemoteDesktopWidget::setInputHandlers(KeyboardInputHandler keyboardHandler,
                                           PointerInputHandler pointerHandler)
{
    keyboardInputHandler = std::move(keyboardHandler);
    pointerInputHandler = std::move(pointerHandler);
}

void RemoteDesktopWidget::setInputEnabled(bool enabled)
{
    if (inputEnabled == enabled) {
        return;
    }

    inputEnabled = enabled;
    if (!inputEnabled) {
        clearInputState();
    }
}

bool RemoteDesktopWidget::event(QEvent *event)
{
    if (inputEnabled && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QFrame::event(event);
}

void RemoteDesktopWidget::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);

    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.fillRect(contentsRect(), QColor(32, 33, 36));

    if (desktopImage.isNull()) {
        painter.setPen(QColor(229, 231, 235));
        painter.drawText(contentsRect().adjusted(16, 16, -16, -16),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         placeholderMessage);
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(desktopTargetRect(), desktopImage);
}

void RemoteDesktopWidget::keyPressEvent(QKeyEvent *event)
{
    handleKeyboardEvent(event);
}

void RemoteDesktopWidget::keyReleaseEvent(QKeyEvent *event)
{
    handleKeyboardEvent(event);
}

void RemoteDesktopWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!inputEnabled || desktopImage.isNull()) {
        QFrame::mouseMoveEvent(event);
        return;
    }

    const bool dragging = forwardedMouseButtons != Qt::NoButton;
    const std::optional<RdpPointerInput> input = InputEventTranslator::mouseMoveInput(
        *event, desktopTargetRect(), desktopImage.size(), dragging);
    if (!input) {
        QFrame::mouseMoveEvent(event);
        return;
    }

    if (dispatchPointerInput(*input)) {
        lastRemotePosition = input->position;
    }
    event->accept();
}

void RemoteDesktopWidget::mousePressEvent(QMouseEvent *event)
{
    handleMouseButtonEvent(event);
}

void RemoteDesktopWidget::mouseReleaseEvent(QMouseEvent *event)
{
    handleMouseButtonEvent(event);
}

void RemoteDesktopWidget::wheelEvent(QWheelEvent *event)
{
    if (!inputEnabled || desktopImage.isNull()) {
        QFrame::wheelEvent(event);
        return;
    }

    const std::optional<RdpPointerInput> input = InputEventTranslator::wheelInput(
        *event, desktopTargetRect(), desktopImage.size());
    if (!input) {
        QFrame::wheelEvent(event);
        return;
    }

    if (dispatchPointerInput(*input)) {
        lastRemotePosition = input->position;
    }
    event->accept();
}

void RemoteDesktopWidget::focusOutEvent(QFocusEvent *event)
{
    if (inputEnabled) {
        releasePressedInputs();
    }
    QFrame::focusOutEvent(event);
}

QRectF RemoteDesktopWidget::desktopTargetRect() const
{
    if (desktopImage.isNull()) {
        return {};
    }

    const QSizeF availableSize = contentsRect().size();
    const QSizeF desktopSize = desktopImage.size();
    const qreal scale = qMin(availableSize.width() / desktopSize.width(),
                             availableSize.height() / desktopSize.height());
    const QSizeF renderedSize = desktopSize * scale;
    const QPointF topLeft(contentsRect().left() + (availableSize.width() - renderedSize.width()) / 2.0,
                          contentsRect().top() + (availableSize.height() - renderedSize.height()) / 2.0);
    return QRectF(topLeft, renderedSize);
}

void RemoteDesktopWidget::updateDirtyRegion(const QRect &dirtyRect)
{
    const QRectF target = desktopTargetRect();
    if (target.isEmpty() || desktopImage.isNull()) {
        update();
        return;
    }

    const qreal scaleX = target.width() / desktopImage.width();
    const qreal scaleY = target.height() / desktopImage.height();
    const QRectF mapped(target.left() + dirtyRect.left() * scaleX,
                        target.top() + dirtyRect.top() * scaleY,
                        dirtyRect.width() * scaleX,
                        dirtyRect.height() * scaleY);
    update(mapped.toAlignedRect().adjusted(-1, -1, 1, 1));
}

void RemoteDesktopWidget::handleKeyboardEvent(QKeyEvent *event)
{
    if (!inputEnabled || desktopImage.isNull()) {
        if (event->type() == QEvent::KeyPress) {
            QFrame::keyPressEvent(event);
        } else {
            QFrame::keyReleaseEvent(event);
        }
        return;
    }

    const std::optional<RdpKeyboardInput> input = InputEventTranslator::keyboardInput(*event);
    if (input && dispatchKeyboardInput(*input)
        && input->kind == RdpKeyboardInput::Kind::ScanCode) {
        if (input->pressed) {
            pressedScanCodes.insert(input->scanCode);
        } else {
            pressedScanCodes.remove(input->scanCode);
        }
    }

    // Keep remote keystrokes from triggering local focus traversal or shortcuts.
    event->accept();
}

void RemoteDesktopWidget::handleMouseButtonEvent(QMouseEvent *event)
{
    if (!inputEnabled || desktopImage.isNull()) {
        if (event->type() == QEvent::MouseButtonPress) {
            QFrame::mousePressEvent(event);
        } else {
            QFrame::mouseReleaseEvent(event);
        }
        return;
    }

    const bool pressed = event->type() == QEvent::MouseButtonPress;
    const bool wasForwarded = forwardedMouseButtons.testFlag(event->button());
    if (!pressed && !wasForwarded) {
        QFrame::mouseReleaseEvent(event);
        return;
    }

    const std::optional<RdpPointerInput> input = InputEventTranslator::mouseButtonInput(
        *event, desktopTargetRect(), desktopImage.size(), !pressed && wasForwarded);
    if (!input) {
        if (pressed) {
            QFrame::mousePressEvent(event);
        } else {
            QFrame::mouseReleaseEvent(event);
        }
        return;
    }

    if (pressed) {
        setFocus(Qt::MouseFocusReason);
    }

    const bool sent = dispatchPointerInput(*input);
    if (sent) {
        lastRemotePosition = input->position;
        if (pressed) {
            forwardedMouseButtons |= event->button();
        }
    }
    if (!pressed) {
        forwardedMouseButtons &= ~event->button();
    }
    event->accept();
}

bool RemoteDesktopWidget::dispatchKeyboardInput(const RdpKeyboardInput &input)
{
    return keyboardInputHandler && keyboardInputHandler(input);
}

bool RemoteDesktopWidget::dispatchPointerInput(const RdpPointerInput &input)
{
    return pointerInputHandler && pointerInputHandler(input);
}

void RemoteDesktopWidget::releasePressedInputs()
{
    const QSet<quint32> scanCodes = pressedScanCodes;
    const Qt::MouseButtons mouseButtons = forwardedMouseButtons;
    clearInputState();

    for (quint32 scanCode : scanCodes) {
        RdpKeyboardInput input;
        input.scanCode = scanCode;
        input.pressed = false;
        if (!dispatchKeyboardInput(input)) {
            return;
        }
    }

    if (mouseButtons.testFlag(Qt::LeftButton)) {
        RdpPointerInput input;
        input.kind = RdpPointerInput::Kind::LeftButton;
        input.position = lastRemotePosition;
        input.pressed = false;
        if (!dispatchPointerInput(input)) {
            return;
        }
    }
    if (mouseButtons.testFlag(Qt::RightButton)) {
        RdpPointerInput input;
        input.kind = RdpPointerInput::Kind::RightButton;
        input.position = lastRemotePosition;
        input.pressed = false;
        (void)dispatchPointerInput(input);
    }
}

void RemoteDesktopWidget::clearInputState()
{
    pressedScanCodes.clear();
    forwardedMouseButtons = Qt::NoButton;
}
