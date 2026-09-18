#ifndef RDPSERVER_H
#define RDPSERVER_H

#include "clipboardcontroller.h"
#include "desktopcapture.h"
#include "inputcontroller.h"
#include "serverconfiguration.h"

#include <QObject>
#include <QString>

#include <memory>

struct RdpServerDependencies
{
    DesktopCaptureFactory desktopCaptureFactory = createDesktopCapture;
    InputControllerFactory inputControllerFactory = createInputController;
    ClipboardControllerFactory clipboardControllerFactory = createClipboardController;
};

class RdpServer : public QObject
{
    Q_OBJECT

public:
    explicit RdpServer(QObject *parent = nullptr);
    explicit RdpServer(RdpServerDependencies dependencies, QObject *parent = nullptr);
    ~RdpServer() override;

    bool start(const RdpServerConfiguration &configuration);
    void stop();

    bool isInitialized() const;
    bool isListening() const;
    qsizetype sessionCount() const;
    QString lastError() const;

signals:
    void listeningStarted(const QString &bindAddress, quint16 port);
    void listeningStopped();
    void clientConnected(quint64 sessionId, const QString &peerAddress);
    void clientDisconnected(quint64 sessionId, const QString &peerAddress);
    void errorOccurred(const QString &message);
    void logMessage(RdpServerLogLevel level,
                    const QString &category,
                    const QString &message);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // RDPSERVER_H
