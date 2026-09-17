#include "rdpserversession_p.h"

#include "rdpclipboardhandler_p.h"
#include "rdpinputhandler_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <freerdp/codec/color.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <freerdp/peer.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/tools/makecert.h>
#include <winpr/wtsapi.h>

#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace {
constexpr DWORD eventPollIntervalMs = 50;
constexpr auto frameInterval = std::chrono::milliseconds(100);
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

RdpServerSession *sessionForInput(rdpInput *input)
{
    return input && input->context ? sessionForPeer(input->context->peer) : nullptr;
}

RdpServerSession *sessionForClipboard(CliprdrServerContext *context)
{
    return context ? static_cast<RdpServerSession *>(context->custom) : nullptr;
}
} // namespace

RdpServerSession::RdpServerSession(quint64 id,
                                   freerdp_peer *peer,
                                   ClosedHandler closedHandler,
                                   ErrorHandler errorHandler,
                                   DesktopCaptureFactory captureFactory,
                                   InputControllerFactory inputFactory,
                                   ClipboardControllerFactory clipboardFactory)
    : sessionId(id)
    , peer(peer)
    , address(peer ? QString::fromUtf8(peer->hostname) : QString())
    , closedHandler(std::move(closedHandler))
    , errorHandler(std::move(errorHandler))
    , captureFactory(std::move(captureFactory))
    , inputFactory(std::move(inputFactory))
    , clipboardFactory(std::move(clipboardFactory))
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
    RdpServerSession *session = sessionForPeer(peer);
    return session && session->handlePostConnect() ? TRUE : FALSE;
}

BOOL RdpServerSession::peerActivate(freerdp_peer *peer)
{
    RdpServerSession *session = sessionForPeer(peer);
    if (!session) {
        return FALSE;
    }
    if (!session->initializeClipboardChannel()) {
        return FALSE;
    }

    session->fullFrameRequested.store(true);
    session->activated.store(true);
    return TRUE;
}

BOOL RdpServerSession::inputKeyboardEvent(rdpInput *input, UINT16 flags, UINT8 code)
{
    RdpServerSession *session = sessionForInput(input);
    if (!session || !session->inputHandler) {
        return FALSE;
    }

    QString errorMessage;
    if (!session->inputHandler->keyboardEvent(flags, code, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply remote keyboard input.")
                                 : errorMessage);
        return FALSE;
    }
    return TRUE;
}

BOOL RdpServerSession::inputUnicodeKeyboardEvent(rdpInput *input, UINT16 flags, UINT16 code)
{
    RdpServerSession *session = sessionForInput(input);
    if (!session || !session->inputHandler) {
        return FALSE;
    }

    QString errorMessage;
    if (!session->inputHandler->unicodeKeyboardEvent(flags, code, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply remote Unicode keyboard input.")
                                 : errorMessage);
        return FALSE;
    }
    return TRUE;
}

BOOL RdpServerSession::inputMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    RdpServerSession *session = sessionForInput(input);
    if (!session || !session->inputHandler) {
        return FALSE;
    }

    QString errorMessage;
    if (!session->inputHandler->mouseEvent(flags, x, y, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply remote mouse input.")
                                 : errorMessage);
        return FALSE;
    }
    return TRUE;
}

BOOL RdpServerSession::inputRelativeMouseEvent(rdpInput *input,
                                               UINT16 flags,
                                               INT16 deltaX,
                                               INT16 deltaY)
{
    RdpServerSession *session = sessionForInput(input);
    if (!session || !session->inputHandler) {
        return FALSE;
    }

    QString errorMessage;
    if (!session->inputHandler->relativeMouseEvent(flags, deltaX, deltaY, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply remote relative mouse input.")
                                 : errorMessage);
        return FALSE;
    }
    return TRUE;
}

BOOL RdpServerSession::inputExtendedMouseEvent(rdpInput *input,
                                               UINT16 flags,
                                               UINT16 x,
                                               UINT16 y)
{
    RdpServerSession *session = sessionForInput(input);
    if (!session || !session->inputHandler) {
        return FALSE;
    }

    QString errorMessage;
    if (!session->inputHandler->extendedMouseEvent(flags, x, y, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply remote extended mouse input.")
                                 : errorMessage);
        return FALSE;
    }
    return TRUE;
}

UINT RdpServerSession::clipboardClientCapabilities(
    CliprdrServerContext *context,
    const CLIPRDR_CAPABILITIES *capabilities)
{
    RdpServerSession *session = sessionForClipboard(context);
    if (!session || !capabilities) {
        return ERROR_INVALID_PARAMETER;
    }
    session->clipboardReady.store(true);
    return CHANNEL_RC_OK;
}

UINT RdpServerSession::clipboardClientFormatList(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_LIST *formatList)
{
    RdpServerSession *session = sessionForClipboard(context);
    if (!session || !session->clipboardHandler || !formatList
        || (formatList->numFormats > 0 && !formatList->formats)) {
        return ERROR_INVALID_PARAMETER;
    }

    const UINT responseResult = session->sendClipboardFormatListResponse(true);
    if (responseResult != CHANNEL_RC_OK) {
        session->reportError(QStringLiteral("Failed to acknowledge the RDP client clipboard "
                                            "format list (channel error 0x%1).")
                                 .arg(responseResult, 8, 16, QLatin1Char('0')));
        return responseResult;
    }

    bool hasUnicodeText = false;
    for (UINT32 index = 0; index < formatList->numFormats; ++index) {
        if (formatList->formats[index].formatId == CF_UNICODETEXT) {
            hasUnicodeText = true;
            break;
        }
    }

    session->requestedClientClipboardFormat = 0;
    if (!hasUnicodeText) {
        QString errorMessage;
        if (!session->clipboardHandler->clearRemoteText(&errorMessage)) {
            session->reportError(errorMessage.isEmpty()
                                     ? QStringLiteral("Failed to clear the server clipboard text.")
                                     : errorMessage);
            return ERROR_INTERNAL_ERROR;
        }
        return CHANNEL_RC_OK;
    }

    const UINT requestResult = session->requestClientClipboardText();
    if (requestResult != CHANNEL_RC_OK) {
        session->reportError(QStringLiteral("Failed to request clipboard text from the RDP client "
                                            "(channel error 0x%1).")
                                 .arg(requestResult, 8, 16, QLatin1Char('0')));
    }
    return requestResult;
}

UINT RdpServerSession::clipboardClientFormatListResponse(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
{
    return sessionForClipboard(context) && formatListResponse ? CHANNEL_RC_OK
                                                               : ERROR_INVALID_PARAMETER;
}

UINT RdpServerSession::clipboardClientFormatDataRequest(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
{
    RdpServerSession *session = sessionForClipboard(context);
    if (!session || !session->clipboardHandler || !formatDataRequest) {
        return ERROR_INVALID_PARAMETER;
    }

    if (formatDataRequest->requestedFormatId != CF_UNICODETEXT) {
        return session->sendClipboardDataResponse(false);
    }

    QByteArray encoded;
    QString errorMessage;
    if (!session->clipboardHandler->encodedLocalText(&encoded, &errorMessage)) {
        const UINT result = session->sendClipboardDataResponse(false);
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to read the server clipboard text.")
                                 : errorMessage);
        return result;
    }
    if (encoded.size() > (std::numeric_limits<UINT32>::max)()) {
        session->reportError(QStringLiteral("The server clipboard text is too large for RDP."));
        return session->sendClipboardDataResponse(false);
    }
    return session->sendClipboardDataResponse(true, encoded);
}

UINT RdpServerSession::clipboardClientFormatDataResponse(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
{
    RdpServerSession *session = sessionForClipboard(context);
    if (!session || !session->clipboardHandler || !formatDataResponse) {
        return ERROR_INVALID_PARAMETER;
    }

    const UINT32 requestedFormat = session->requestedClientClipboardFormat;
    session->requestedClientClipboardFormat = 0;
    if (requestedFormat != CF_UNICODETEXT
        || (formatDataResponse->common.msgFlags & CB_RESPONSE_FAIL) != 0) {
        return CHANNEL_RC_OK;
    }

    const UINT32 dataLength = formatDataResponse->common.dataLen;
    if ((dataLength > 0 && !formatDataResponse->requestedFormatData)
        || dataLength > static_cast<quint64>((std::numeric_limits<qsizetype>::max)())) {
        session->reportError(QStringLiteral("The RDP client returned invalid clipboard data."));
        return ERROR_INVALID_DATA;
    }

    QByteArray encoded;
    if (dataLength > 0) {
        encoded = QByteArray(
            reinterpret_cast<const char *>(formatDataResponse->requestedFormatData),
            static_cast<qsizetype>(dataLength));
    }
    QString errorMessage;
    if (!session->clipboardHandler->applyRemoteText(encoded, &errorMessage)) {
        session->reportError(errorMessage.isEmpty()
                                 ? QStringLiteral("Failed to apply the RDP client clipboard text.")
                                 : errorMessage);
        return ERROR_INVALID_DATA;
    }
    return CHANNEL_RC_OK;
}

bool RdpServerSession::handlePostConnect()
{
    if (!desktopCapture) {
        reportError(QStringLiteral("Desktop capture is not initialized."));
        return false;
    }

    QString errorMessage;
    const DesktopSize size = desktopCapture->desktopSize(&errorMessage);
    if (!size.isValid()) {
        reportError(errorMessage.isEmpty()
                        ? QStringLiteral("Failed to query the desktop resolution.")
                        : errorMessage);
        return false;
    }
    if (!applyDesktopSize(size, true)) {
        return false;
    }
    return true;
}

bool RdpServerSession::initializePeer()
{
    if (!peer) {
        return false;
    }

    desktopCapture = captureFactory ? captureFactory() : nullptr;
    if (!desktopCapture) {
        reportError(QStringLiteral("Desktop capture is not available on this platform."));
        return false;
    }

    inputController = inputFactory ? inputFactory() : nullptr;
    if (!inputController) {
        reportError(QStringLiteral("Remote input is not available on this platform."));
        return false;
    }
    inputHandler = std::make_unique<RdpInputHandler>(*inputController);

    clipboardController = clipboardFactory ? clipboardFactory() : nullptr;
    if (!clipboardController) {
        reportError(QStringLiteral("Clipboard synchronization is not available on this platform."));
        return false;
    }
    clipboardHandler = std::make_unique<RdpClipboardHandler>(*clipboardController);
    QString clipboardError;
    if (!clipboardHandler->initialize(&clipboardError)) {
        reportError(clipboardError.isEmpty()
                        ? QStringLiteral("Failed to initialize clipboard synchronization.")
                        : clipboardError);
        return false;
    }

    QString captureError;
    const DesktopSize captureSize = desktopCapture->desktopSize(&captureError);
    if (!captureSize.isValid()
        || captureSize.width > (std::numeric_limits<UINT16>::max)()
        || captureSize.height > (std::numeric_limits<UINT16>::max)()) {
        reportError(captureError.isEmpty()
                        ? QStringLiteral("The desktop resolution is invalid or exceeds the RDP limit.")
                        : captureError);
        return false;
    }

    peer->ContextExtra = this;
    if (!freerdp_peer_context_new(peer)) {
        return false;
    }
    peerInitialized = true;
    if (!peer->context || !peer->context->settings || !peer->context->input) {
        return false;
    }

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
        || !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, captureSize.width)
        || !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, captureSize.height)
        || !freerdp_settings_set_uint32(settings,
                                        FreeRDP_MultifragMaxRequestSize,
                                        0x00FFFFFF)
        || !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, FALSE)) {
        return false;
    }

    desktopWidth = captureSize.width;
    desktopHeight = captureSize.height;

    nscContext = nsc_context_new();
    frameStream = Stream_New(nullptr, initialStreamCapacity);
    if (!nscContext || !frameStream
        || !nsc_context_set_parameters(nscContext, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRA32)) {
        return false;
    }

    peer->PostConnect = peerPostConnect;
    peer->Activate = peerActivate;
    rdpInput *input = peer->context->input;
    input->KeyboardEvent = inputKeyboardEvent;
    input->UnicodeKeyboardEvent = inputUnicodeKeyboardEvent;
    input->MouseEvent = inputMouseEvent;
    input->RelMouseEvent = inputRelativeMouseEvent;
    input->ExtendedMouseEvent = inputExtendedMouseEvent;
    return peer->Initialize && peer->Initialize(peer);
}

void RdpServerSession::cleanupPeer()
{
    activated.store(false);
    cleanupClipboardChannel();

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
    if (virtualChannelManager) {
        WTSCloseServer(virtualChannelManager);
        virtualChannelManager = nullptr;
    }
    if (peerInitialized && peer) {
        freerdp_peer_context_free(peer);
        peerInitialized = false;
    }
    if (peer) {
        peer->ContextExtra = nullptr;
    }
    inputHandler.reset();
    inputController.reset();
    clipboardHandler.reset();
    clipboardController.reset();
    desktopCapture.reset();
}

bool RdpServerSession::initializeClipboardChannel()
{
    if (clipboardStarted || clipboardContext) {
        return true;
    }
    if (!peer || !peer->context) {
        reportError(QStringLiteral("The RDP peer is not initialized for clipboard redirection."));
        return false;
    }
    if (!WTSIsChannelJoinedByName(peer, CLIPRDR_SVC_CHANNEL_NAME)) {
        return true;
    }
    if (!virtualChannelManager) {
        virtualChannelManager = WTSOpenServerA(reinterpret_cast<LPSTR>(peer->context));
        if (!virtualChannelManager || virtualChannelManager == INVALID_HANDLE_VALUE) {
            virtualChannelManager = nullptr;
            reportError(QStringLiteral("Failed to create the RDP virtual channel manager."));
            return false;
        }
    }
    clipboardContext = cliprdr_server_context_new(virtualChannelManager);
    if (!clipboardContext) {
        reportError(QStringLiteral("Failed to create the FreeRDP clipboard channel."));
        return false;
    }
    clipboardContext->custom = this;
    clipboardContext->rdpcontext = peer ? peer->context : nullptr;
    clipboardContext->useLongFormatNames = TRUE;
    clipboardContext->streamFileClipEnabled = FALSE;
    clipboardContext->fileClipNoFilePaths = FALSE;
    clipboardContext->canLockClipData = FALSE;
    clipboardContext->autoInitializationSequence = TRUE;
    clipboardContext->ClientCapabilities = clipboardClientCapabilities;
    clipboardContext->ClientFormatList = clipboardClientFormatList;
    clipboardContext->ClientFormatListResponse = clipboardClientFormatListResponse;
    clipboardContext->ClientFormatDataRequest = clipboardClientFormatDataRequest;
    clipboardContext->ClientFormatDataResponse = clipboardClientFormatDataResponse;

    const UINT result = clipboardContext->Start
                            ? clipboardContext->Start(clipboardContext)
                            : ERROR_INVALID_PARAMETER;
    if (result != CHANNEL_RC_OK) {
        reportError(QStringLiteral("Failed to start the FreeRDP clipboard channel "
                                   "(channel error 0x%1).")
                        .arg(result, 8, 16, QLatin1Char('0')));
        clipboardContext->custom = nullptr;
        cliprdr_server_context_free(clipboardContext);
        clipboardContext = nullptr;
        return false;
    }
    clipboardStarted = true;
    return true;
}

void RdpServerSession::cleanupClipboardChannel()
{
    clipboardReady.store(false);
    requestedClientClipboardFormat = 0;
    if (!clipboardContext) {
        clipboardStarted = false;
        return;
    }

    if (clipboardStarted && clipboardContext->Stop) {
        (void)clipboardContext->Stop(clipboardContext);
    }
    clipboardStarted = false;
    clipboardContext->custom = nullptr;
    cliprdr_server_context_free(clipboardContext);
    clipboardContext = nullptr;
}

bool RdpServerSession::pollClipboard()
{
    if (!clipboardReady.load() || !clipboardHandler || !clipboardContext) {
        return true;
    }

    ClipboardTextState state;
    QString errorMessage;
    const ClipboardPollStatus status = clipboardHandler->pollLocalClipboard(&state,
                                                                            &errorMessage);
    if (status == ClipboardPollStatus::Unchanged) {
        return true;
    }
    if (status == ClipboardPollStatus::Error) {
        reportError(errorMessage.isEmpty()
                        ? QStringLiteral("Failed to read a local clipboard change.")
                        : errorMessage);
        return true;
    }

    const UINT result = sendClipboardFormatList(state);
    if (result != CHANNEL_RC_OK) {
        reportError(QStringLiteral("Failed to announce the server clipboard text "
                                   "(channel error 0x%1).")
                        .arg(result, 8, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

UINT RdpServerSession::sendClipboardFormatList(const ClipboardTextState &state)
{
    if (!clipboardContext || !clipboardContext->ServerFormatList) {
        return ERROR_INVALID_PARAMETER;
    }

    CLIPRDR_FORMAT unicodeTextFormat = {};
    unicodeTextFormat.formatId = CF_UNICODETEXT;
    CLIPRDR_FORMAT_LIST formatList = {};
    formatList.common.msgType = CB_FORMAT_LIST;
    formatList.numFormats = state.hasText ? 1 : 0;
    formatList.formats = state.hasText ? &unicodeTextFormat : nullptr;

    std::lock_guard<std::mutex> lock(clipboardSendMutex);
    return clipboardContext->ServerFormatList(clipboardContext, &formatList);
}

UINT RdpServerSession::sendClipboardFormatListResponse(bool accepted)
{
    if (!clipboardContext || !clipboardContext->ServerFormatListResponse) {
        return ERROR_INVALID_PARAMETER;
    }

    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = accepted ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    std::lock_guard<std::mutex> lock(clipboardSendMutex);
    return clipboardContext->ServerFormatListResponse(clipboardContext, &response);
}

UINT RdpServerSession::requestClientClipboardText()
{
    if (!clipboardContext || !clipboardContext->ServerFormatDataRequest) {
        return ERROR_INVALID_PARAMETER;
    }

    CLIPRDR_FORMAT_DATA_REQUEST request = {};
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.requestedFormatId = CF_UNICODETEXT;
    requestedClientClipboardFormat = CF_UNICODETEXT;
    std::lock_guard<std::mutex> lock(clipboardSendMutex);
    const UINT result = clipboardContext->ServerFormatDataRequest(clipboardContext, &request);
    if (result != CHANNEL_RC_OK) {
        requestedClientClipboardFormat = 0;
    }
    return result;
}

UINT RdpServerSession::sendClipboardDataResponse(bool accepted, const QByteArray &encoded)
{
    if (!clipboardContext || !clipboardContext->ServerFormatDataResponse) {
        return ERROR_INVALID_PARAMETER;
    }

    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = accepted ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = accepted ? static_cast<UINT32>(encoded.size()) : 0;
    response.requestedFormatData = accepted
                                       ? reinterpret_cast<const BYTE *>(encoded.constData())
                                       : nullptr;
    std::lock_guard<std::mutex> lock(clipboardSendMutex);
    return clipboardContext->ServerFormatDataResponse(clipboardContext, &response);
}

bool RdpServerSession::applyDesktopSize(const DesktopSize &size, bool notifyClient)
{
    if (!size.isValid() || size.width > (std::numeric_limits<UINT16>::max)()
        || size.height > (std::numeric_limits<UINT16>::max)() || !peer || !peer->context
        || !peer->context->settings) {
        reportError(QStringLiteral("The captured desktop resolution cannot be used by RDP."));
        return false;
    }

    rdpSettings *settings = peer->context->settings;
    const UINT32 configuredWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const UINT32 configuredHeight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    desktopWidth = size.width;
    desktopHeight = size.height;
    if (configuredWidth == size.width && configuredHeight == size.height) {
        return true;
    }

    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, size.width)
        || !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, size.height)) {
        reportError(QStringLiteral("Failed to update the RDP desktop resolution."));
        return false;
    }

    if (!notifyClient) {
        return true;
    }

    rdpUpdate *update = peer->context->update;
    if (!update || !update->DesktopResize || !update->DesktopResize(update->context)) {
        reportError(QStringLiteral("Failed to notify the RDP client of a desktop resolution change."));
        return false;
    }
    return true;
}

bool RdpServerSession::sendDesktopFrame(bool forceFullFrame)
{
    if (!peer || !peer->context || !peer->context->settings || !peer->context->update
        || !nscContext || !frameStream || !desktopCapture) {
        reportError(QStringLiteral("The desktop frame pipeline is not initialized."));
        return false;
    }

    rdpSettings *settings = peer->context->settings;
    rdpUpdate *update = peer->context->update;
    const UINT32 codecId = freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId);
    if (codecId == 0 || codecId > (std::numeric_limits<UINT16>::max)()
        || !update->SurfaceBits) {
        reportError(QStringLiteral("The RDP client did not negotiate a usable desktop codec."));
        return false;
    }

    DesktopCaptureResult captureResult = desktopCapture->capture(forceFullFrame);
    if (captureResult.status == DesktopCaptureStatus::Error) {
        reportError(captureResult.errorMessage.isEmpty()
                        ? QStringLiteral("Desktop capture failed.")
                        : captureResult.errorMessage);
        return false;
    }
    if (captureResult.status == DesktopCaptureStatus::NoChanges) {
        return true;
    }

    const DesktopFrame &frame = captureResult.frame;
    if (!frame.isValid()) {
        reportError(QStringLiteral("Desktop capture returned an invalid frame."));
        return false;
    }

    if (frame.desktopSize.width != desktopWidth || frame.desktopSize.height != desktopHeight) {
        activated.store(false);
        fullFrameRequested.store(true);
        return applyDesktopSize(frame.desktopSize, true);
    }

    Stream_Clear(frameStream);
    Stream_ResetPosition(frameStream);
    if (!nsc_compose_message(nscContext,
                             frameStream,
                             frame.pixels.data(),
                             static_cast<UINT32>(frame.width),
                             static_cast<UINT32>(frame.height),
                             frame.stride)) {
        reportError(QStringLiteral("Failed to encode the captured desktop frame."));
        return false;
    }

    const size_t encodedSize = Stream_GetPosition(frameStream);
    if (encodedSize == 0 || encodedSize > (std::numeric_limits<UINT32>::max)()) {
        reportError(QStringLiteral("The encoded desktop frame has an invalid size."));
        return false;
    }

    SURFACE_BITS_COMMAND command = {};
    command.cmdType = CMDTYPE_SET_SURFACE_BITS;
    command.destLeft = frame.x;
    command.destTop = frame.y;
    command.destRight = frame.x + frame.width;
    command.destBottom = frame.y + frame.height;
    command.bmp.bpp = 32;
    command.bmp.codecID = static_cast<UINT16>(codecId);
    command.bmp.width = static_cast<UINT16>(frame.width);
    command.bmp.height = static_cast<UINT16>(frame.height);
    command.bmp.bitmapDataLength = static_cast<UINT32>(encodedSize);
    command.bmp.bitmapData = Stream_Buffer(frameStream);
    command.skipCompression = FALSE;

    if (!update->SurfaceBits(update->context, &command)) {
        reportError(QStringLiteral("Failed to send the captured desktop frame to the RDP client."));
        return false;
    }

    return true;
}

void RdpServerSession::reportError(const QString &message)
{
    if (errorReported.exchange(true)) {
        return;
    }
    if (errorHandler) {
        errorHandler(sessionId, message);
    }
}

void RdpServerSession::run()
{
    if (initializePeer()) {
        auto nextFrame = std::chrono::steady_clock::now();

        while (!stopRequested.load()) {
            std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles = {};
            const bool hasVirtualChannelManager = virtualChannelManager != nullptr;
            const DWORD maximumPeerHandles = static_cast<DWORD>(
                handles.size() - (hasVirtualChannelManager ? 1 : 0));
            const DWORD handleCount = peer->GetEventHandles
                                          ? peer->GetEventHandles(peer,
                                                                  handles.data(),
                                                                  maximumPeerHandles)
                                          : 0;
            if (handleCount == 0) {
                break;
            }

            DWORD totalHandleCount = handleCount;
            DWORD virtualChannelHandleIndex = MAXIMUM_WAIT_OBJECTS;
            if (hasVirtualChannelManager) {
                const HANDLE channelEvent =
                    WTSVirtualChannelManagerGetEventHandle(virtualChannelManager);
                if (!channelEvent) {
                    break;
                }
                virtualChannelHandleIndex = totalHandleCount;
                handles[totalHandleCount++] = channelEvent;
            }

            const DWORD waitResult = WaitForMultipleObjects(totalHandleCount,
                                                             handles.data(),
                                                             FALSE,
                                                             eventPollIntervalMs);
            if (waitResult == WAIT_FAILED) {
                break;
            }
            if (waitResult != WAIT_TIMEOUT) {
                if (waitResult >= WAIT_OBJECT_0 + totalHandleCount) {
                    break;
                }

                const DWORD signaledHandle = waitResult - WAIT_OBJECT_0;
                if (signaledHandle == virtualChannelHandleIndex) {
                    if (!WTSVirtualChannelManagerCheckFileDescriptorEx(
                            virtualChannelManager, FALSE)) {
                        break;
                    }
                } else if (!peer->CheckFileDescriptor
                           || !peer->CheckFileDescriptor(peer)) {
                    break;
                }
            }

            if (!pollClipboard()) {
                break;
            }

            if (!activated.load()) {
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool fullFrame = fullFrameRequested.exchange(false);
            if ((fullFrame || now >= nextFrame) && !sendDesktopFrame(fullFrame)) {
                break;
            }
            if (fullFrame || now >= nextFrame) {
                nextFrame = now + frameInterval;
            }
        }
    } else {
        reportError(QStringLiteral("Failed to initialize the RDP client session."));
    }

    cleanupPeer();
    running.store(false);
    if (closedHandler) {
        closedHandler(sessionId, address);
    }
}
