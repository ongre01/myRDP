#ifndef RDPCLIENT_H
#define RDPCLIENT_H

#include <QByteArray>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>

struct CertificateInfo
{
    QString host;
    int port = 0;
    QString commonName;
    QString subject;
    QString issuer;
    QString fingerprint;
    QString oldSubject;
    QString oldIssuer;
    QString oldFingerprint;
    bool hostNameMismatch = false;
    bool changed = false;
};

enum class CertificateDecision
{
    Reject,
    TrustOnce
};

struct ConnectionInfo
{
    QString serverAddress;
    int port = 3389;
    QString username;
    QString password;
    QString domain;
    std::function<CertificateDecision(const CertificateInfo &)> certificateVerifier;
};

struct DesktopUpdate
{
    QSize desktopSize;
    QRect dirtyRect;
    QByteArray pixels;
    int bytesPerLine = 0;
};

class RdpClient
{
public:
    RdpClient();
    ~RdpClient();

    RdpClient(const RdpClient &) = delete;
    RdpClient &operator=(const RdpClient &) = delete;
    RdpClient(RdpClient &&) = delete;
    RdpClient &operator=(RdpClient &&) = delete;

    bool connectToServer(const ConnectionInfo &info);
    void disconnect();
    bool processEvents();
    void setDesktopUpdateHandler(std::function<void(const DesktopUpdate &)> handler);

    bool isInitialized() const;
    bool isConnected() const;
    QString lastError() const;
    static QString libraryVersion();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // RDPCLIENT_H
