#ifndef RDPSERVERSESSION_P_H
#define RDPSERVERSESSION_P_H

#include <QString>
#include <QtTypes>

#include <atomic>
#include <functional>
#include <thread>

#include <winpr/stream.h>
#include <winpr/wtypes.h>

struct rdp_freerdp_peer;
using freerdp_peer = struct rdp_freerdp_peer;
struct S_NSC_CONTEXT;
using NSC_CONTEXT = struct S_NSC_CONTEXT;

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
    static BOOL peerPostConnect(freerdp_peer *peer);
    static BOOL peerActivate(freerdp_peer *peer);

    bool initializePeer();
    void cleanupPeer();
    bool sendTestFrame(bool fullFrame);
    void run();

    quint64 sessionId;
    freerdp_peer *peer;
    QString address;
    ClosedHandler closedHandler;
    bool ownsPeer = false;
    bool started = false;
    bool peerInitialized = false;
    std::atomic_bool stopRequested = false;
    std::atomic_bool running = false;
    std::atomic_bool activated = false;
    std::atomic_bool fullFrameRequested = false;
    NSC_CONTEXT *nscContext = nullptr;
    wStream *frameStream = nullptr;
    quint64 frameNumber = 0;
    quint32 desktopWidth = 0;
    quint32 desktopHeight = 0;
    std::thread worker;
};

#endif // RDPSERVERSESSION_P_H
