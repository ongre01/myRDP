#ifndef RDPCLIENT_H
#define RDPCLIENT_H

#include <QString>

#include <memory>

struct ConnectionInfo
{
    QString serverAddress;
    int port = 3389;
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

    bool isInitialized() const;
    bool isConnected() const;
    QString lastError() const;
    static QString libraryVersion();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

#endif // RDPCLIENT_H
