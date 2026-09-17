#ifndef RDPSERVER_H
#define RDPSERVER_H

#include <QObject>
#include <QString>

#include <memory>

struct RdpServerConfiguration
{
    QString bindAddress = QStringLiteral("0.0.0.0");
    quint16 port = 3389;
};

class RdpServer : public QObject
{
    Q_OBJECT

public:
    explicit RdpServer(QObject *parent = nullptr);
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

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // RDPSERVER_H
