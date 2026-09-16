#include "remotedesktopwidget.h"

#include <QPaintEvent>
#include <QPainter>

RemoteDesktopWidget::RemoteDesktopWidget(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(false);
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
