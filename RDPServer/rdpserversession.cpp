#include "rdpserversession_p.h"

#include <freerdp/peer.h>
#include <winpr/winsock.h>

#include <cerrno>
#include <chrono>
#include <utility>

namespace {
constexpr auto disconnectPollInterval = std::chrono::milliseconds(100);

bool isRetryableSocketError()
{
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEINTR || error == WSAEWOULDBLOCK;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
} // namespace

RdpServerSession::RdpServerSession(quint64 id,
                                   freerdp_peer *peer,
                                   ClosedHandler closedHandler)
    : sessionId(id)
    , peer(peer)
    , address(peer ? QString::fromUtf8(peer->hostname) : QString())
    , closedHandler(std::move(closedHandler))
{
}

RdpServerSession::~RdpServerSession()
{
    requestStop();
    join();

    if (ownsPeer) {
        freerdp_peer_free(peer);
    }
}

void RdpServerSession::takePeerOwnership()
{
    ownsPeer = true;
}

bool RdpServerSession::start()
{
    if (started || !ownsPeer || !peer || peer->sockfd < 0) {
        return false;
    }

    started = true;
    running.store(true);

    try {
        worker = std::thread(&RdpServerSession::run, this);
    } catch (...) {
        running.store(false);
        started = false;
        return false;
    }

    return true;
}

void RdpServerSession::requestStop()
{
    stopRequested.store(true);
    waitCondition.notify_all();
}

void RdpServerSession::join()
{
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
        worker.join();
    }
}

quint64 RdpServerSession::id() const
{
    return sessionId;
}

QString RdpServerSession::peerAddress() const
{
    return address;
}

bool RdpServerSession::isRunning() const
{
    return running.load();
}

bool RdpServerSession::peerDisconnected() const
{
    const SOCKET socket = static_cast<SOCKET>(peer->sockfd);
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval timeout = {};
    const int selected = select(static_cast<int>(socket + 1), &readSet, nullptr, nullptr, &timeout);
    if (selected == 0) {
        return false;
    }
    if (selected == SOCKET_ERROR) {
        return !isRetryableSocketError();
    }

    char buffer[4096] = {};
    const int received = recv(socket, buffer, sizeof(buffer), 0);
    if (received > 0) {
        return false;
    }
    if (received == 0) {
        return true;
    }

    return !isRetryableSocketError();
}

void RdpServerSession::run()
{
    while (!stopRequested.load()) {
        std::unique_lock<std::mutex> lock(waitMutex);
        waitCondition.wait_for(lock,
                               disconnectPollInterval,
                               [this]() { return stopRequested.load(); });
        lock.unlock();

        if (stopRequested.load() || peerDisconnected()) {
            break;
        }
    }

    running.store(false);
    if (closedHandler) {
        closedHandler(sessionId, address);
    }
}
