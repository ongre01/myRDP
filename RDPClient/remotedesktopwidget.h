#ifndef REMOTEDESKTOPWIDGET_H
#define REMOTEDESKTOPWIDGET_H

#include <QFrame>
#include <QImage>

class RemoteDesktopWidget : public QFrame
{
    Q_OBJECT

public:
    explicit RemoteDesktopWidget(QWidget *parent = nullptr);

    void setPlaceholderMessage(const QString &message);
    void updateDesktopRegion(const QSize &desktopSize,
                             const QRect &dirtyRect,
                             const QImage &regionImage);
    void clearDesktop();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF desktopTargetRect() const;
    void updateDirtyRegion(const QRect &dirtyRect);

    QImage desktopImage;
    QString placeholderMessage;
};

#endif // REMOTEDESKTOPWIDGET_H
