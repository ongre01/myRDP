#include "rdpserversession_p.h"

#include "rdptestframe_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <freerdp/codec/color.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/tools/makecert.h>

#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace {
constexpr DWORD eventPollIntervalMs = 50;
constexpr auto frameInterval = std::chrono::milliseconds(500);
constexpr std::size_t initialStreamCapacity = 64 * 1024;
constexpr auto testCertificateBaseName = "qtrdp-test";

class TestServerCredentials final
{
public:
    TestServerCredentials()
    {
        if (!directory.isValid()) {
            return;
        }

        MAKECERT_CONTEXT *context = makecert_context_new();
        if (!context) {
            return;
        }

        char executable[] = "makecert";
        char rdpOption[] = "-rdp";
        char liveOption[] = "-live";
        char silentOption[] = "-silent";
        char yearsOption[] = "-y";
        char yearsValue[] = "1";
        char *arguments[] = {
            executable,
            rdpOption,
            liveOption,
            silentOption,
            yearsOption,
            yearsValue,
        };

        const QByteArray outputDirectory = QFile::encodeName(directory.path());
        const bool generated = makecert_context_process(context, 6, arguments) >= 0
                               && makecert_context_set_output_file_name(
                                      context,
                                      testCertificateBaseName)
                                      == 1
                               && makecert_context_output_certificate_file(
                                      context,
                                      outputDirectory.constData())
                                      == 1
                               && makecert_context_output_private_key_file(
                                      context,
                                      outputDirectory.constData())
                                      == 1;
        makecert_context_free(context);

        if (!generated) {
            return;
        }

        certificateFile = QDir(directory.path())
                              .filePath(QStringLiteral("qtrdp-test.crt"));
        privateKeyFile = QDir(directory.path())
                             .filePath(QStringLiteral("qtrdp-test.key"));
        valid = QFileInfo::exists(certificateFile) && QFileInfo::exists(privateKeyFile);
    }

    bool isValid() const
    {
        return valid;
    }

    QByteArray encodedCertificateFile() const
    {
        return QFile::encodeName(certificateFile);
    }

    QByteArray encodedPrivateKeyFile() const
    {
        return QFile::encodeName(privateKeyFile);
    }

private:
    QTemporaryDir directory;
    QString certificateFile;
    QString privateKeyFile;
    bool valid = false;
};

const TestServerCredentials &testServerCredentials()
{
    static const TestServerCredentials credentials;
    return credentials;
}

RdpServerSession *sessionForPeer(freerdp_peer *peer)
{
    return peer ? static_cast<RdpServerSession *>(peer->ContextExtra) : nullptr;
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
    cleanupPeer();

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

BOOL RdpServerSession::peerPostConnect(freerdp_peer *peer)
{
    return sessionForPeer(peer) ? TRUE : FALSE;
}

BOOL RdpServerSession::peerActivate(freerdp_peer *peer)
{
    RdpServerSession *session = sessionForPeer(peer);
    if (!session) {
        return FALSE;
    }

    session->fullFrameRequested.store(true);
    session->activated.store(true);
    return TRUE;
}

bool RdpServerSession::initializePeer()
{
    if (!peer) {
        return false;
    }

    peer->ContextExtra = this;
    if (!freerdp_peer_context_new(peer) || !peer->context || !peer->context->settings) {
        return false;
    }
    peerInitialized = true;

    rdpSettings *settings = peer->context->settings;
    const TestServerCredentials &credentials = testServerCredentials();
    if (!credentials.isValid()) {
        return false;
    }

    const QByteArray privateKeyFile = credentials.encodedPrivateKeyFile();
    rdpPrivateKey *privateKey = freerdp_key_new_from_file_enc(privateKeyFile.constData(), nullptr);
    if (!privateKey
        || !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, privateKey, 1)) {
        freerdp_key_free(privateKey);
        return false;
    }

    const QByteArray certificateFile = credentials.encodedCertificateFile();
    rdpCertificate *certificate = freerdp_certificate_new_from_file(certificateFile.constData());
    if (!certificate
        || !freerdp_settings_set_pointer_len(settings,
                                             FreeRDP_RdpServerCertificate,
                                             certificate,
                                             1)) {
        freerdp_certificate_free(certificate);
        return false;
    }

    if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE)
        || !freerdp_settings_set_uint32(settings,
                                        FreeRDP_EncryptionLevel,
                                        ENCRYPTION_LEVEL_CLIENT_COMPATIBLE)
        || !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE)
        || !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32)
        || !freerdp_settings_set_uint32(settings,
                                        FreeRDP_MultifragMaxRequestSize,
                                        0x00FFFFFF)
        || !freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, FALSE)) {
        return false;
    }

    nscContext = nsc_context_new();
    frameStream = Stream_New(nullptr, initialStreamCapacity);
    if (!nscContext || !frameStream
        || !nsc_context_set_parameters(nscContext, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRA32)) {
        return false;
    }

    peer->PostConnect = peerPostConnect;
    peer->Activate = peerActivate;
    return peer->Initialize && peer->Initialize(peer);
}

void RdpServerSession::cleanupPeer()
{
    activated.store(false);

    if (peerInitialized && peer && peer->context && peer->Disconnect) {
        peer->Disconnect(peer);
    }
    if (frameStream) {
        Stream_Free(frameStream, TRUE);
        frameStream = nullptr;
    }
    if (nscContext) {
        nsc_context_free(nscContext);
        nscContext = nullptr;
    }
    if (peerInitialized && peer) {
        freerdp_peer_context_free(peer);
        peerInitialized = false;
    }
    if (peer) {
        peer->ContextExtra = nullptr;
    }
}

bool RdpServerSession::sendTestFrame(bool fullFrame)
{
    if (!peer || !peer->context || !peer->context->settings || !peer->context->update
        || !nscContext || !frameStream) {
        return false;
    }

    rdpSettings *settings = peer->context->settings;
    rdpUpdate *update = peer->context->update;
    const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    const UINT32 codecId = freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId);
    if (width == 0 || height == 0 || width > (std::numeric_limits<UINT16>::max)()
        || height > (std::numeric_limits<UINT16>::max)() || codecId == 0
        || codecId > (std::numeric_limits<UINT16>::max)() || !update->SurfaceBits) {
        return false;
    }

    if (width != desktopWidth || height != desktopHeight) {
        desktopWidth = width;
        desktopHeight = height;
        fullFrame = true;
    }

    const UINT16 frameWidth = static_cast<UINT16>(width);
    const UINT16 frameHeight = static_cast<UINT16>(height);
    RdpTestFrame frame = fullFrame
                             ? RdpTestFrameGenerator::fullFrame(frameWidth,
                                                                frameHeight,
                                                                frameNumber)
                             : RdpTestFrameGenerator::counterFrame(frameWidth,
                                                                   frameHeight,
                                                                   frameNumber);
    if (!frame.isValid()) {
        return false;
    }

    Stream_Clear(frameStream);
    Stream_ResetPosition(frameStream);
    if (!nsc_compose_message(nscContext,
                             frameStream,
                             frame.pixels.data(),
                             frame.width,
                             frame.height,
                             frame.stride)) {
        return false;
    }

    const size_t encodedSize = Stream_GetPosition(frameStream);
    if (encodedSize == 0 || encodedSize > (std::numeric_limits<UINT32>::max)()) {
        return false;
    }

    SURFACE_BITS_COMMAND command = {};
    command.cmdType = CMDTYPE_SET_SURFACE_BITS;
    command.destLeft = frame.x;
    command.destTop = frame.y;
    command.destRight = static_cast<UINT32>(frame.x) + frame.width;
    command.destBottom = static_cast<UINT32>(frame.y) + frame.height;
    command.bmp.bpp = 32;
    command.bmp.codecID = static_cast<UINT16>(codecId);
    command.bmp.width = frame.width;
    command.bmp.height = frame.height;
    command.bmp.bitmapDataLength = static_cast<UINT32>(encodedSize);
    command.bmp.bitmapData = Stream_Buffer(frameStream);
    command.skipCompression = FALSE;

    if (!update->SurfaceBits(update->context, &command)) {
        return false;
    }

    ++frameNumber;
    return true;
}

void RdpServerSession::run()
{
    if (initializePeer()) {
        auto nextFrame = std::chrono::steady_clock::now();

        while (!stopRequested.load()) {
            std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles = {};
            const DWORD handleCount = peer->GetEventHandles
                                          ? peer->GetEventHandles(peer,
                                                                  handles.data(),
                                                                  static_cast<DWORD>(handles.size()))
                                          : 0;
            if (handleCount == 0) {
                break;
            }

            const DWORD waitResult = WaitForMultipleObjects(handleCount,
                                                            handles.data(),
                                                            FALSE,
                                                            eventPollIntervalMs);
            if (waitResult == WAIT_FAILED) {
                break;
            }
            if (waitResult != WAIT_TIMEOUT) {
                if (waitResult >= WAIT_OBJECT_0 + handleCount
                    || !peer->CheckFileDescriptor
                    || !peer->CheckFileDescriptor(peer)) {
                    break;
                }
            }

            if (!activated.load()) {
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool fullFrame = fullFrameRequested.exchange(false);
            if ((fullFrame || now >= nextFrame) && !sendTestFrame(fullFrame)) {
                break;
            }
            if (fullFrame || now >= nextFrame) {
                nextFrame = now + frameInterval;
            }
        }
    }

    cleanupPeer();
    running.store(false);
    if (closedHandler) {
        closedHandler(sessionId, address);
    }
}
