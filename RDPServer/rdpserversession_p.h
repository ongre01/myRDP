#ifndef RDPSERVERSESSION_P_H
#define RDPSERVERSESSION_P_H

#include <QString>
#include <QtTypes>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

struct rdp_freerdp_peer;
using freerdp_peer = struct rdp_freerdp_peer;

class RdpServerSession final
{
public:
    using ClosedHandler = std::function<void(quint64, const QString &)>;

    RdpServerSession(quint64 id, freerdp_peer *peer, ClosedHandler closedHandler);
    ~RdpServerSession();

    RdpServerSession(const RdpServerSession &) = delete;
    RdpServerSession &operator=(const RdpServerSession &) = delete;

    void takePeerOwnership();
    bool start();
    void requestStop();
    void join();

    quint64 id() const;
    QString peerAddress() const;
    bool isRunning() const;

private:
    bool peerDisconnected() const;
    void run();

    quint64 sessionId;
    freerdp_peer *peer;
    QString address;
    ClosedHandler closedHandler;
    bool ownsPeer = false;
    bool started = false;
    std::atomic_bool stopRequested = false;
    std::atomic_bool running = false;
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::thread worker;
};

#endif // RDPSERVERSESSION_P_H
