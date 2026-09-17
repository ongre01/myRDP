#include "rdpserver.h"

#include "rdpserversession_p.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>

#include <freerdp/listener.h>
#include <winpr/synch.h>
#include <winpr/winsock.h>

#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
void configureOpenSslProviderPath()
{
#if defined(Q_OS_WIN)
    static std::once_flag configured;
    std::call_once(configured, []() {
        const QString applicationDirectory = QCoreApplication::applicationDirPath();
        const QString legacyProvider =
            QDir(applicationDirectory).filePath(QStringLiteral("legacy.dll"));
        QString providerDirectory;

        if (!qEnvironmentVariableIsEmpty("OPENSSL_MODULES")) {
            providerDirectory = QFile::decodeName(qgetenv("OPENSSL_MODULES"));
        } else if (QFileInfo::exists(legacyProvider)) {
            providerDirectory = applicationDirectory;
            qputenv("OPENSSL_MODULES", QFile::encodeName(providerDirectory));
        }

        if (providerDirectory.isEmpty()) {
            return;
        }

        QLibrary cryptoLibrary(
            QDir(applicationDirectory).filePath(QStringLiteral("libcrypto-3-x64.dll")));
        if (!cryptoLibrary.load()) {
            return;
        }

        using SetProviderSearchPath = int (*)(void *, const char *);
        const auto setProviderSearchPath = reinterpret_cast<SetProviderSearchPath>(
            cryptoLibrary.resolve("OSSL_PROVIDER_set_default_search_path"));
        if (setProviderSearchPath) {
            const QByteArray encodedProviderDirectory = QFile::encodeName(providerDirectory);
            setProviderSearchPath(nullptr, encodedProviderDirectory.constData());
        }

        using LoadProvider = void *(*)(void *, const char *);
        const auto loadProvider = reinterpret_cast<LoadProvider>(
            cryptoLibrary.resolve("OSSL_PROVIDER_load"));
        if (loadProvider) {
            static void *defaultProvider = loadProvider(nullptr, "default");
            static void *legacyProvider = loadProvider(nullptr, "legacy");
            Q_UNUSED(defaultProvider);
            Q_UNUSED(legacyProvider);
        }
    });
#endif
}
} // namespace

class RdpServer::Impl
{
public:
    explicit Impl(RdpServer *owner)
        : owner(owner)
    {
        configureOpenSslProviderPath();

#if defined(Q_OS_WIN)
        WSADATA socketData = {};
        if (WSAStartup(MAKEWORD(2, 2), &socketData) != 0) {
            setError(RdpServer::tr("Windows socket initialization failed."));
            return;
        }
        socketInitialized = true;
#endif

        listener = freerdp_listener_new();
        if (!listener) {
            setError(RdpServer::tr("FreeRDP server listener initialization failed."));
            return;
        }

        stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent) {
            setError(RdpServer::tr("Server stop event initialization failed."));
            return;
        }

        listener->info = this;
        listener->PeerAccepted = peerAccepted;
    }

    ~Impl()
    {
        stop();

        if (stopEvent) {
            CloseHandle(stopEvent);
        }
        if (listener) {
            freerdp_listener_free(listener);
        }
#if defined(Q_OS_WIN)
        if (socketInitialized) {
            WSACleanup();
        }
#endif
    }

    bool start(const RdpServerConfiguration &configuration)
    {
        if (!isInitialized()) {
            return false;
        }
        if (configuration.port == 0) {
            return fail(RdpServer::tr("The listen port must be between 1 and 65535."));
        }
        if (listening.load()) {
            return fail(RdpServer::tr("The RDP server is already listening."));
        }

        if (listenerThread.joinable()) {
            listenerThread.join();
        }
        collectFinishedSessions();

        const QString normalizedAddress = configuration.bindAddress.trimmed();
        const QByteArray nativeAddress = normalizedAddress.toUtf8();
        const char *bindAddress = normalizedAddress.isEmpty() ? nullptr : nativeAddress.constData();

        clearError();
        stopping.store(false);
        ResetEvent(stopEvent);

        if (!listener->Open(listener, bindAddress, configuration.port)) {
            return fail(RdpServer::tr("Failed to listen on %1:%2 (socket error %3).")
                            .arg(displayAddress(normalizedAddress))
                            .arg(configuration.port)
                            .arg(WSAGetLastError()));
        }

        currentAddress = normalizedAddress;
        currentPort = configuration.port;
        listening.store(true);

        try {
            listenerThread = std::thread(&Impl::listenerLoop, this);
        } catch (const std::exception &exception) {
            listener->Close(listener);
            listening.store(false);
            return fail(RdpServer::tr("Failed to start the listener thread: %1")
                            .arg(QString::fromLocal8Bit(exception.what())));
        } catch (...) {
            listener->Close(listener);
            listening.store(false);
            return fail(RdpServer::tr("Failed to start the listener thread."));
        }

        emit owner->listeningStarted(displayAddress(currentAddress), currentPort);
        return true;
    }

    void stop()
    {
        const bool wasListening = listening.load();
        stopping.store(true);
        if (stopEvent) {
            SetEvent(stopEvent);
        }

        requestAllSessionsStop();

        if (listenerThread.joinable()
            && listenerThread.get_id() != std::this_thread::get_id()) {
            listenerThread.join();
        }

        std::vector<std::unique_ptr<RdpServerSession>> remainingSessions;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex);
            remainingSessions.reserve(sessions.size());
            for (auto &entry : sessions) {
                remainingSessions.push_back(std::move(entry.second));
            }
            sessions.clear();
        }

        for (const auto &session : remainingSessions) {
            session->requestStop();
        }
        for (const auto &session : remainingSessions) {
            session->join();
        }

        listening.store(false);
        stopping.store(false);
        if (wasListening) {
            emit owner->listeningStopped();
        }
    }

    bool isInitialized() const
    {
#if defined(Q_OS_WIN)
        return socketInitialized && listener && stopEvent;
#else
        return listener && stopEvent;
#endif
    }

    qsizetype sessionCount() const
    {
        qsizetype activeSessions = 0;
        std::lock_guard<std::mutex> lock(sessionsMutex);
        for (const auto &entry : sessions) {
            if (entry.second->isRunning()) {
                ++activeSessions;
            }
        }
        return activeSessions;
    }

    QString lastError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return errorMessage;
    }

    static BOOL peerAccepted(freerdp_listener *listener, freerdp_peer *peer)
    {
        if (!listener || !listener->info || !peer) {
            return FALSE;
        }

        auto *server = static_cast<Impl *>(listener->info);
        return server->acceptPeer(peer) ? TRUE : FALSE;
    }

    bool acceptPeer(freerdp_peer *peer)
    {
        const quint64 id = nextSessionId.fetch_add(1);
        RdpServerSession *newSession = nullptr;
        std::unique_ptr<RdpServerSession> failedSession;

        try {
            std::lock_guard<std::mutex> lock(sessionsMutex);
            if (stopping.load()) {
                return false;
            }

            auto session = std::make_unique<RdpServerSession>(
                id,
                peer,
                [this](quint64 closedId, const QString &peerAddress) {
                    emit owner->clientDisconnected(closedId, peerAddress);
                },
                [this](quint64 failedId, const QString &message) {
                    const QString sessionError = RdpServer::tr("Session %1: %2")
                                                     .arg(failedId)
                                                     .arg(message);
                    setError(sessionError);
                    emit owner->errorOccurred(sessionError);
                });
            newSession = session.get();
            sessions.emplace(id, std::move(session));
            newSession->takePeerOwnership();

            if (!newSession->start()) {
                auto iterator = sessions.find(id);
                failedSession = std::move(iterator->second);
                sessions.erase(iterator);
            }
        } catch (const std::exception &exception) {
            setError(RdpServer::tr("Failed to create a client session: %1")
                         .arg(QString::fromLocal8Bit(exception.what())));
            emit owner->errorOccurred(lastError());
            return false;
        } catch (...) {
            setError(RdpServer::tr("Failed to create a client session."));
            emit owner->errorOccurred(lastError());
            return false;
        }

        if (failedSession) {
            setError(RdpServer::tr("Failed to start client session %1.").arg(id));
            emit owner->errorOccurred(lastError());
            return true;
        }

        emit owner->clientConnected(id, newSession->peerAddress());
        return true;
    }

    void listenerLoop()
    {
        QString loopError;

        while (!stopping.load()) {
            std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles = {};
            const DWORD listenerHandleCount = listener->GetEventHandles(
                listener,
                handles.data(),
                static_cast<DWORD>(handles.size() - 1));

            if (listenerHandleCount == 0) {
                loopError = RdpServer::tr("FreeRDP returned no listener event handles.");
                break;
            }

            handles[listenerHandleCount] = stopEvent;
            const DWORD waitResult = WaitForMultipleObjects(
                listenerHandleCount + 1,
                handles.data(),
                FALSE,
                250);

            if (waitResult == WAIT_TIMEOUT) {
                collectFinishedSessions();
                continue;
            }
            if (waitResult == WAIT_OBJECT_0 + listenerHandleCount) {
                break;
            }
            if (waitResult == WAIT_FAILED) {
                loopError = RdpServer::tr("Waiting for a FreeRDP listener event failed.");
                break;
            }
            if (waitResult >= WAIT_OBJECT_0 + listenerHandleCount) {
                loopError = RdpServer::tr("FreeRDP returned an unexpected listener event.");
                break;
            }

            if (!listener->CheckFileDescriptor(listener)) {
                if (!stopping.load()) {
                    loopError = RdpServer::tr("FreeRDP failed while accepting a client connection.");
                }
                break;
            }

            collectFinishedSessions();
        }

        listener->Close(listener);
        collectFinishedSessions();
        listening.store(false);

        if (!loopError.isEmpty() && !stopping.load()) {
            setError(loopError);
            emit owner->errorOccurred(loopError);
        }
        if (!stopping.load()) {
            emit owner->listeningStopped();
        }
    }

    void requestAllSessionsStop()
    {
        std::lock_guard<std::mutex> lock(sessionsMutex);
        for (const auto &entry : sessions) {
            entry.second->requestStop();
        }
    }

    void collectFinishedSessions()
    {
        std::vector<std::unique_ptr<RdpServerSession>> finishedSessions;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex);
            for (auto iterator = sessions.begin(); iterator != sessions.end();) {
                if (iterator->second->isRunning()) {
                    ++iterator;
                    continue;
                }

                finishedSessions.push_back(std::move(iterator->second));
                iterator = sessions.erase(iterator);
            }
        }

        for (const auto &session : finishedSessions) {
            session->join();
        }
    }

    static QString displayAddress(const QString &address)
    {
        return address.isEmpty() ? RdpServer::tr("all interfaces") : address;
    }

    bool fail(const QString &message)
    {
        setError(message);
        return false;
    }

    void setError(const QString &message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        errorMessage = message;
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        errorMessage.clear();
    }

    RdpServer *owner;
    freerdp_listener *listener = nullptr;
    HANDLE stopEvent = nullptr;
    std::atomic_bool listening = false;
    std::atomic_bool stopping = false;
    std::atomic<quint64> nextSessionId = 1;
    QString currentAddress;
    quint16 currentPort = 0;
    std::thread listenerThread;
    mutable std::mutex sessionsMutex;
    std::unordered_map<quint64, std::unique_ptr<RdpServerSession>> sessions;
    mutable std::mutex errorMutex;
    QString errorMessage;
#if defined(Q_OS_WIN)
    bool socketInitialized = false;
#endif
};

RdpServer::RdpServer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>(this))
{
}

RdpServer::~RdpServer()
{
    d->stop();
}

bool RdpServer::start(const RdpServerConfiguration &configuration)
{
    return d->start(configuration);
}

void RdpServer::stop()
{
    d->stop();
}

bool RdpServer::isInitialized() const
{
    return d->isInitialized();
}

bool RdpServer::isListening() const
{
    return d->listening.load();
}

qsizetype RdpServer::sessionCount() const
{
    return d->sessionCount();
}

QString RdpServer::lastError() const
{
    return d->lastError();
}
