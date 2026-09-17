#include "rdpclient.h"
#include "clipboardtextcodec.h"

#include <QByteArray>

#include <cstring>
#include <limits>
#include <utility>

#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/config.h>
#include <freerdp/error.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/error.h>
#include <winpr/winsock.h>

namespace {

struct ClientContext
{
    rdpContext context;
    void *owner;
};

QString connectionError(const rdpContext *context)
{
    if (!context) {
        return QStringLiteral("FreeRDP connection failed without an initialized context.");
    }

    const UINT32 errorCode = freerdp_get_last_error(context);
    if (errorCode == FREERDP_ERROR_SUCCESS) {
        return QStringLiteral("The RDP connection was closed by the remote server.");
    }

    const char *errorName = freerdp_get_last_error_name(errorCode);
    const char *errorDescription = freerdp_get_last_error_string(errorCode);
    const QString hexadecimalCode = QString::number(errorCode, 16)
                                        .rightJustified(8, QLatin1Char('0'))
                                        .toUpper();

    const QString details = QStringLiteral("%1 (0x%2): %3")
        .arg(errorName ? QString::fromLatin1(errorName) : QStringLiteral("FREERDP_ERROR_UNKNOWN"),
             hexadecimalCode,
             errorDescription ? QString::fromLatin1(errorDescription)
                              : QStringLiteral("No error description is available."));

    switch (errorCode) {
    case FREERDP_ERROR_AUTHENTICATION_FAILED:
    case FREERDP_ERROR_INSUFFICIENT_PRIVILEGES:
    case FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED:
    case FREERDP_ERROR_CONNECT_PASSWORD_CERTAINLY_EXPIRED:
    case FREERDP_ERROR_CONNECT_CLIENT_REVOKED:
    case FREERDP_ERROR_CONNECT_KDC_UNREACHABLE:
    case FREERDP_ERROR_CONNECT_ACCOUNT_DISABLED:
    case FREERDP_ERROR_CONNECT_PASSWORD_MUST_CHANGE:
    case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
    case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
    case FREERDP_ERROR_CONNECT_ACCESS_DENIED:
    case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION:
    case FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT:
    case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
    case FREERDP_ERROR_CONNECT_LOGON_TYPE_NOT_GRANTED:
    case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
        return QStringLiteral("Authentication failed. Check the user name, password, domain, "
                              "and Remote Desktop access. %1")
            .arg(details);
    case FREERDP_ERROR_DNS_ERROR:
    case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
        return QStringLiteral("The RDP server name could not be resolved. %1").arg(details);
    case FREERDP_ERROR_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
        return QStringLiteral("The RDP server could not be reached. %1").arg(details);
    default:
        return details;
    }
}

} // namespace

class RdpClient::Impl
{
public:
    Impl()
    {
#if defined(_WIN32)
        WSADATA socketData = {};
        const int socketResult = WSAStartup(MAKEWORD(2, 2), &socketData);
        if (socketResult != 0) {
            error = QStringLiteral("Windows socket initialization failed (WSAStartup error %1).")
                        .arg(socketResult);
            return;
        }
        winsockInitialized = true;
#endif

        instance = freerdp_new();
        if (!instance) {
            error = QStringLiteral("FreeRDP instance initialization failed.");
            return;
        }

        instance->ContextSize = sizeof(ClientContext);
        instance->PreConnect = preConnect;
        instance->PostConnect = postConnect;
        instance->PostDisconnect = postDisconnect;
        instance->VerifyCertificateEx = verifyCertificate;
        instance->VerifyChangedCertificateEx = verifyChangedCertificate;

        if (!freerdp_context_new(instance)) {
            error = QStringLiteral("FreeRDP context initialization failed.");
            freerdp_free(instance);
            instance = nullptr;
            return;
        }

#if defined(WITH_CLIENT_CHANNELS)
        if (freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0)
            != CHANNEL_RC_OK) {
            error = QStringLiteral("FreeRDP static channel provider initialization failed.");
            freerdp_context_free(instance);
            freerdp_free(instance);
            instance = nullptr;
            return;
        }
#endif

        clientContext(instance)->owner = this;
    }

    ~Impl()
    {
        disconnect();

        if (instance) {
            freerdp_context_free(instance);
            freerdp_free(instance);
        }

#if defined(_WIN32)
        if (winsockInitialized) {
            WSACleanup();
        }
#endif
    }

    bool connectToServer(const ConnectionInfo &info)
    {
        if (!isInitialized()) {
            if (error.isEmpty()) {
                error = QStringLiteral("FreeRDP is not initialized.");
            }
            return false;
        }

        if (connected) {
            error = QStringLiteral("An RDP connection is already active.");
            return false;
        }

        const QString serverAddress = info.serverAddress.trimmed();
        if (serverAddress.isEmpty()) {
            error = QStringLiteral("The RDP server address is empty.");
            return false;
        }

        if (info.port < 1 || info.port > 65535) {
            error = QStringLiteral("The RDP server port must be between 1 and 65535.");
            return false;
        }

        const QString username = info.username.trimmed();
        if (username.isEmpty()) {
            error = QStringLiteral("The RDP user name is empty.");
            return false;
        }

        rdpSettings *settings = instance->context->settings;
        const QByteArray encodedAddress = serverAddress.toUtf8();
        const QByteArray encodedUsername = username.toUtf8();
        const QByteArray encodedPassword = info.password.toUtf8();
        const QByteArray encodedDomain = info.domain.trimmed().toUtf8();

        if (!freerdp_settings_set_string(settings,
                                         FreeRDP_ServerHostname,
                                         encodedAddress.constData())
            || !freerdp_settings_set_uint32(settings,
                                            FreeRDP_ServerPort,
                                            static_cast<UINT32>(info.port))
            || !freerdp_settings_set_string(settings,
                                            FreeRDP_Username,
                                            encodedUsername.constData())
            || !freerdp_settings_set_string(settings,
                                            FreeRDP_Password,
                                            encodedPassword.constData())
            || !freerdp_settings_set_string(settings,
                                            FreeRDP_Domain,
                                            encodedDomain.constData())
            || !freerdp_settings_set_bool(settings, FreeRDP_Authentication, TRUE)
            || !freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE)
            || !freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE)) {
            error = QStringLiteral("FreeRDP connection settings could not be configured.");
            return false;
        }

        certificateVerifier = info.certificateVerifier;
        certificateRejected = false;
        error.clear();
        if (!freerdp_connect(instance)) {
            if (certificateRejected) {
                error = QStringLiteral("The RDP server certificate was rejected.");
            } else if (error.isEmpty()) {
                error = connectionError(instance->context);
            }
            unsubscribeChannelEvents();
            certificateVerifier = {};
            return false;
        }

        certificateVerifier = {};
        connected = true;
        return true;
    }

    void disconnect()
    {
        if (!connected || !isInitialized()) {
            return;
        }

        error.clear();
        if (!freerdp_disconnect(instance)) {
            error = connectionError(instance->context);
        }
        connected = false;
    }

    bool processEvents()
    {
        if (!connected || !isInitialized()) {
            return true;
        }

        if (freerdp_check_event_handles(instance->context)) {
            return true;
        }

        const QString eventError = error.isEmpty() ? connectionError(instance->context) : error;
        (void)freerdp_disconnect(instance);
        connected = false;
        error = eventError;
        return false;
    }

    bool sendKeyboardInput(const RdpKeyboardInput &input)
    {
        if (!hasActiveInput()) {
            error = QStringLiteral("Keyboard input cannot be sent without an active RDP connection.");
            return false;
        }

        BOOL sent = FALSE;
        switch (input.kind) {
        case RdpKeyboardInput::Kind::ScanCode:
            if (input.scanCode == RDP_SCANCODE_UNKNOWN) {
                error = QStringLiteral("The keyboard event has an unknown RDP scan code.");
                return false;
            }
            sent = freerdp_input_send_keyboard_event_ex(instance->context->input,
                                                        input.pressed ? TRUE : FALSE,
                                                        input.repeat ? TRUE : FALSE,
                                                        input.scanCode);
            break;
        case RdpKeyboardInput::Kind::Pause:
            if (!input.pressed) {
                return true;
            }
            sent = freerdp_input_send_keyboard_pause_event(instance->context->input);
            break;
        }

        if (!sent) {
            error = QStringLiteral("FreeRDP could not send the keyboard input event.");
            return false;
        }
        return true;
    }

    bool sendPointerInput(const RdpPointerInput &input)
    {
        if (!hasActiveInput()) {
            error = QStringLiteral("Pointer input cannot be sent without an active RDP connection.");
            return false;
        }

        if (input.position.x() < 0 || input.position.y() < 0
            || input.position.x() > (std::numeric_limits<UINT16>::max)()
            || input.position.y() > (std::numeric_limits<UINT16>::max)()) {
            error = QStringLiteral("The pointer coordinates are outside the RDP coordinate range.");
            return false;
        }

        const UINT16 x = static_cast<UINT16>(input.position.x());
        const UINT16 y = static_cast<UINT16>(input.position.y());
        switch (input.kind) {
        case RdpPointerInput::Kind::Move:
            return sendMouseEvent(PTR_FLAGS_MOVE, x, y);
        case RdpPointerInput::Kind::LeftButton:
            return sendMouseEvent(PTR_FLAGS_BUTTON1
                                      | (input.pressed ? PTR_FLAGS_DOWN : 0),
                                  x,
                                  y);
        case RdpPointerInput::Kind::RightButton:
            return sendMouseEvent(PTR_FLAGS_BUTTON2
                                      | (input.pressed ? PTR_FLAGS_DOWN : 0),
                                  x,
                                  y);
        case RdpPointerInput::Kind::VerticalWheel:
            return sendWheelEvent(input.wheelDelta, x, y);
        }

        error = QStringLiteral("The pointer input event type is unsupported.");
        return false;
    }

    bool sendClipboardText(const QString &text)
    {
        localClipboardText = text;
        if (!clipboardContext || !clipboardReady) {
            return true;
        }

        const UINT result = sendLocalClipboardFormatList();
        if (result != CHANNEL_RC_OK) {
            error = QStringLiteral("FreeRDP could not announce the local clipboard text "
                                   "(channel error 0x%1).")
                        .arg(result, 8, 16, QLatin1Char('0'));
            return false;
        }
        return true;
    }

    void setDesktopUpdateHandler(std::function<void(const DesktopUpdate &)> handler)
    {
        desktopUpdateHandler = std::move(handler);
    }

    void setClipboardTextHandler(std::function<void(const QString &)> handler)
    {
        clipboardTextHandler = std::move(handler);
    }

    bool isInitialized() const
    {
        return instance && instance->context;
    }

private:
    UINT sendClientCapabilities()
    {
        if (!clipboardContext || !clipboardContext->ClientCapabilities) {
            return ERROR_INVALID_PARAMETER;
        }

        CLIPRDR_GENERAL_CAPABILITY_SET generalCapability = {};
        generalCapability.capabilitySetType = CB_CAPSTYPE_GENERAL;
        generalCapability.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
        generalCapability.version = CB_CAPS_VERSION_2;
        generalCapability.generalFlags = CB_USE_LONG_FORMAT_NAMES;

        CLIPRDR_CAPABILITIES capabilities = {};
        capabilities.common.msgType = CB_CLIP_CAPS;
        capabilities.cCapabilitiesSets = 1;
        capabilities.capabilitySets =
            reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&generalCapability);
        return clipboardContext->ClientCapabilities(clipboardContext, &capabilities);
    }

    UINT sendLocalClipboardFormatList()
    {
        if (!clipboardContext || !clipboardContext->ClientFormatList) {
            return ERROR_INVALID_PARAMETER;
        }

        CLIPRDR_FORMAT unicodeTextFormat = {};
        unicodeTextFormat.formatId = CF_UNICODETEXT;

        CLIPRDR_FORMAT_LIST formatList = {};
        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.numFormats = 1;
        formatList.formats = &unicodeTextFormat;
        return clipboardContext->ClientFormatList(clipboardContext, &formatList);
    }

    UINT sendFormatListResponse(bool accepted)
    {
        if (!clipboardContext || !clipboardContext->ClientFormatListResponse) {
            return ERROR_INVALID_PARAMETER;
        }

        CLIPRDR_FORMAT_LIST_RESPONSE response = {};
        response.common.msgType = CB_FORMAT_LIST_RESPONSE;
        response.common.msgFlags = accepted ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
        return clipboardContext->ClientFormatListResponse(clipboardContext, &response);
    }

    UINT requestRemoteClipboardText()
    {
        if (!clipboardContext || !clipboardContext->ClientFormatDataRequest) {
            return ERROR_INVALID_PARAMETER;
        }

        CLIPRDR_FORMAT_DATA_REQUEST request = {};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.requestedFormatId = CF_UNICODETEXT;
        requestedRemoteFormat = CF_UNICODETEXT;
        const UINT result = clipboardContext->ClientFormatDataRequest(clipboardContext, &request);
        if (result != CHANNEL_RC_OK) {
            requestedRemoteFormat = 0;
        }
        return result;
    }

    UINT publishRemoteClipboardText(const QString &text)
    {
        if (!clipboardTextHandler) {
            return CHANNEL_RC_OK;
        }

        try {
            clipboardTextHandler(text);
        } catch (...) {
            error = QStringLiteral("The remote clipboard text handler failed.");
            return ERROR_INTERNAL_ERROR;
        }
        return CHANNEL_RC_OK;
    }

    void attachClipboardChannel(CliprdrClientContext *context)
    {
        if (!context) {
            return;
        }

        clipboardContext = context;
        clipboardReady = false;
        requestedRemoteFormat = 0;
        context->custom = this;
        context->MonitorReady = monitorReady;
        context->ServerCapabilities = serverCapabilities;
        context->ServerFormatList = serverFormatList;
        context->ServerFormatListResponse = serverFormatListResponse;
        context->ServerFormatDataRequest = serverFormatDataRequest;
        context->ServerFormatDataResponse = serverFormatDataResponse;
    }

    void detachClipboardChannel(CliprdrClientContext *context)
    {
        if (!context || context != clipboardContext) {
            return;
        }

        context->custom = nullptr;
        clipboardContext = nullptr;
        clipboardReady = false;
        requestedRemoteFormat = 0;
    }

    void unsubscribeChannelEvents()
    {
        if (!channelEventsSubscribed || !instance || !instance->context
            || !instance->context->pubSub) {
            clipboardContext = nullptr;
            clipboardReady = false;
            requestedRemoteFormat = 0;
            channelEventsSubscribed = false;
            return;
        }

        PubSub_UnsubscribeChannelConnected(instance->context->pubSub, channelConnected);
        PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub, channelDisconnected);
        channelEventsSubscribed = false;
        clipboardContext = nullptr;
        clipboardReady = false;
        requestedRemoteFormat = 0;
    }

    bool hasActiveInput() const
    {
        return connected && instance && instance->context && instance->context->input;
    }

    bool sendMouseEvent(UINT16 flags, UINT16 x, UINT16 y)
    {
        if (!freerdp_input_send_mouse_event(instance->context->input, flags, x, y)) {
            error = QStringLiteral("FreeRDP could not send the pointer input event.");
            return false;
        }
        return true;
    }

    bool sendWheelEvent(int delta, UINT16 x, UINT16 y)
    {
        if (delta == 0) {
            return true;
        }

        qint64 remaining = delta;
        const bool negative = remaining < 0;
        if (negative) {
            remaining = -remaining;
        }

        while (remaining > 0) {
            const UINT16 amount = static_cast<UINT16>(qMin<qint64>(remaining, 0xFF));
            UINT16 flags = PTR_FLAGS_WHEEL;
            UINT16 encodedAmount = amount;
            if (negative) {
                flags |= PTR_FLAGS_WHEEL_NEGATIVE;
                encodedAmount = static_cast<UINT16>(0x100 - amount);
            }
            flags |= encodedAmount & WheelRotationMask;
            if (!sendMouseEvent(flags, x, y)) {
                return false;
            }
            remaining -= amount;
        }
        return true;
    }

    bool loadClipboardChannel()
    {
#if defined(CHANNEL_CLIPRDR_CLIENT)
        const DWORD flags = FREERDP_ADDIN_CHANNEL_STATIC | FREERDP_ADDIN_CHANNEL_ENTRYEX;
        const PVIRTUALCHANNELENTRY rawEntry = freerdp_load_channel_addin_entry(
            CLIPRDR_SVC_CHANNEL_NAME, nullptr, nullptr, flags);
        const auto entry = reinterpret_cast<PVIRTUALCHANNELENTRYEX>(rawEntry);
        if (!entry) {
            error = QStringLiteral("The FreeRDP cliprdr client entry point is unavailable.");
            return false;
        }

        if (freerdp_channels_client_load_ex(instance->context->channels,
                                            instance->context->settings,
                                            entry,
                                            nullptr)
            != 0) {
            error = QStringLiteral("FreeRDP could not initialize the cliprdr client channel.");
            return false;
        }
        return true;
#else
        error = QStringLiteral("This FreeRDP installation was built without the cliprdr client "
                               "channel. Run scripts/build-freerdp.ps1 and rebuild RDPClient.");
        return false;
#endif
    }

    static BOOL preConnect(freerdp *instance)
    {
        Impl *implementation = owner(instance);
        if (!implementation || !instance->context || !instance->context->settings
            || !instance->context->channels || !instance->context->pubSub) {
            return FALSE;
        }

        rdpSettings *settings = instance->context->settings;
        if (!freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE)
            || !freerdp_settings_set_bool(settings, FreeRDP_DesktopResize, TRUE)) {
            return FALSE;
        }

        if (PubSub_SubscribeChannelConnected(instance->context->pubSub, channelConnected) < 0) {
            return FALSE;
        }
        if (PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
                                                channelDisconnected) < 0) {
            PubSub_UnsubscribeChannelConnected(instance->context->pubSub, channelConnected);
            return FALSE;
        }
        implementation->channelEventsSubscribed = true;

        if (!implementation->loadClipboardChannel()) {
            implementation->unsubscribeChannelEvents();
            return FALSE;
        }
        return TRUE;
    }

    static BOOL postConnect(freerdp *instance)
    {
        Impl *implementation = owner(instance);
        if (!implementation || !instance->context || !instance->context->update) {
            return FALSE;
        }

        if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
            implementation->error = QStringLiteral("FreeRDP framebuffer initialization failed.");
            return FALSE;
        }

        rdpUpdate *update = instance->context->update;
        update->BeginPaint = beginPaint;
        update->EndPaint = endPaint;
        update->DesktopResize = desktopResize;

        const rdpGdi *gdi = instance->context->gdi;
        return implementation->publishFramebuffer(
                   instance->context,
                   QRect(0, 0, gdi ? gdi->width : 0, gdi ? gdi->height : 0))
                   ? TRUE
                   : FALSE;
    }

    static void postDisconnect(freerdp *instance)
    {
        Impl *implementation = owner(instance);
        if (implementation) {
            implementation->unsubscribeChannelEvents();
        }
        gdi_free(instance);
    }

    static void channelConnected(void *context, const ChannelConnectedEventArgs *event)
    {
        Impl *implementation = owner(context);
        if (!implementation || !event || !event->name) {
            return;
        }

        if (std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            implementation->attachClipboardChannel(
                static_cast<CliprdrClientContext *>(event->pInterface));
            return;
        }
        freerdp_client_OnChannelConnectedEventHandler(context, event);
    }

    static void channelDisconnected(void *context, const ChannelDisconnectedEventArgs *event)
    {
        Impl *implementation = owner(context);
        if (!implementation || !event || !event->name) {
            return;
        }

        if (std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            implementation->detachClipboardChannel(
                static_cast<CliprdrClientContext *>(event->pInterface));
            return;
        }
        freerdp_client_OnChannelDisconnectedEventHandler(context, event);
    }

    static UINT monitorReady(CliprdrClientContext *context,
                             const CLIPRDR_MONITOR_READY *monitorReady)
    {
        Impl *implementation = clipboardOwner(context);
        if (!implementation || !monitorReady) {
            return ERROR_INVALID_PARAMETER;
        }

        UINT result = implementation->sendClientCapabilities();
        if (result != CHANNEL_RC_OK) {
            return result;
        }

        implementation->clipboardReady = true;
        result = implementation->sendLocalClipboardFormatList();
        if (result != CHANNEL_RC_OK) {
            implementation->clipboardReady = false;
        }
        return result;
    }

    static UINT serverCapabilities(CliprdrClientContext *context,
                                   const CLIPRDR_CAPABILITIES *capabilities)
    {
        return clipboardOwner(context) && capabilities ? CHANNEL_RC_OK
                                                       : ERROR_INVALID_PARAMETER;
    }

    static UINT serverFormatList(CliprdrClientContext *context,
                                 const CLIPRDR_FORMAT_LIST *formatList)
    {
        Impl *implementation = clipboardOwner(context);
        if (!implementation || !formatList
            || (formatList->numFormats > 0 && !formatList->formats)) {
            return ERROR_INVALID_PARAMETER;
        }

        bool hasUnicodeText = false;
        for (UINT32 index = 0; index < formatList->numFormats; ++index) {
            if (formatList->formats[index].formatId == CF_UNICODETEXT) {
                hasUnicodeText = true;
                break;
            }
        }

        const UINT responseResult = implementation->sendFormatListResponse(true);
        if (responseResult != CHANNEL_RC_OK) {
            return responseResult;
        }

        if (formatList->numFormats == 0) {
            implementation->requestedRemoteFormat = 0;
            return implementation->publishRemoteClipboardText(QString());
        }
        if (!hasUnicodeText) {
            implementation->requestedRemoteFormat = 0;
            return CHANNEL_RC_OK;
        }
        return implementation->requestRemoteClipboardText();
    }

    static UINT serverFormatListResponse(
        CliprdrClientContext *context,
        const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
    {
        return clipboardOwner(context) && formatListResponse ? CHANNEL_RC_OK
                                                             : ERROR_INVALID_PARAMETER;
    }

    static UINT serverFormatDataRequest(
        CliprdrClientContext *context,
        const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
    {
        Impl *implementation = clipboardOwner(context);
        if (!implementation || !formatDataRequest || !context->ClientFormatDataResponse) {
            return ERROR_INVALID_PARAMETER;
        }

        const bool supported = formatDataRequest->requestedFormatId == CF_UNICODETEXT;
        const QByteArray encoded = supported
                                       ? ClipboardTextCodec::encodeUtf16Le(
                                             implementation->localClipboardText)
                                       : QByteArray();
        if (encoded.size() > (std::numeric_limits<UINT32>::max)()) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        CLIPRDR_FORMAT_DATA_RESPONSE response = {};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = supported ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
        response.common.dataLen = supported ? static_cast<UINT32>(encoded.size()) : 0;
        response.requestedFormatData = supported
                                           ? reinterpret_cast<const BYTE *>(encoded.constData())
                                           : nullptr;
        return context->ClientFormatDataResponse(context, &response);
    }

    static UINT serverFormatDataResponse(
        CliprdrClientContext *context,
        const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
    {
        Impl *implementation = clipboardOwner(context);
        if (!implementation || !formatDataResponse) {
            return ERROR_INVALID_PARAMETER;
        }

        const UINT32 requestedFormat = implementation->requestedRemoteFormat;
        implementation->requestedRemoteFormat = 0;
        if (requestedFormat != CF_UNICODETEXT) {
            return CHANNEL_RC_OK;
        }
        if ((formatDataResponse->common.msgFlags & CB_RESPONSE_FAIL) != 0) {
            return CHANNEL_RC_OK;
        }

        const UINT32 dataLength = formatDataResponse->common.dataLen;
        if (dataLength > 0 && !formatDataResponse->requestedFormatData) {
            return ERROR_INVALID_DATA;
        }

        QByteArray encoded;
        if (dataLength > 0) {
            encoded = QByteArray(
                reinterpret_cast<const char *>(formatDataResponse->requestedFormatData),
                static_cast<qsizetype>(dataLength));
        }
        bool decodedSuccessfully = false;
        const QString text = ClipboardTextCodec::decodeUtf16Le(encoded, &decodedSuccessfully);
        if (!decodedSuccessfully) {
            implementation->error = QStringLiteral("The remote clipboard returned invalid "
                                                   "UTF-16 text data.");
            return ERROR_INVALID_DATA;
        }
        return implementation->publishRemoteClipboardText(text);
    }

    static BOOL beginPaint(rdpContext *context)
    {
        if (!context || !context->gdi || !context->gdi->primary
            || !context->gdi->primary->hdc || !context->gdi->primary->hdc->hwnd
            || !context->gdi->primary->hdc->hwnd->invalid) {
            return FALSE;
        }

        HGDI_WND window = context->gdi->primary->hdc->hwnd;
        window->invalid->null = TRUE;
        window->ninvalid = 0;
        return TRUE;
    }

    static BOOL endPaint(rdpContext *context)
    {
        Impl *implementation = context && context->instance ? owner(context->instance) : nullptr;
        if (!implementation || !context->gdi || !context->gdi->primary
            || !context->gdi->primary->hdc || !context->gdi->primary->hdc->hwnd) {
            return FALSE;
        }

        HGDI_WND window = context->gdi->primary->hdc->hwnd;
        if (!window->invalid || window->invalid->null) {
            return TRUE;
        }

        const GDI_RGN *invalid = window->invalid;
        return implementation->publishFramebuffer(
                   context,
                   QRect(invalid->x, invalid->y, invalid->w, invalid->h))
                   ? TRUE
                   : FALSE;
    }

    static BOOL desktopResize(rdpContext *context)
    {
        Impl *implementation = context && context->instance ? owner(context->instance) : nullptr;
        if (!implementation || !context->gdi || !context->settings) {
            return FALSE;
        }

        const UINT32 width =
            freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
        const UINT32 height =
            freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
        if (!gdi_resize(context->gdi, width, height)) {
            implementation->error = QStringLiteral("The remote desktop framebuffer could not be resized.");
            return FALSE;
        }

        return implementation->publishFramebuffer(
                   context,
                   QRect(0, 0, context->gdi->width, context->gdi->height))
                   ? TRUE
                   : FALSE;
    }

    bool publishFramebuffer(rdpContext *context, const QRect &requestedRect)
    {
        if (!desktopUpdateHandler) {
            return true;
        }

        if (!context || !context->gdi || !context->gdi->primary_buffer
            || context->gdi->width <= 0 || context->gdi->height <= 0) {
            error = QStringLiteral("The FreeRDP framebuffer is unavailable.");
            return false;
        }

        const rdpGdi *gdi = context->gdi;
        const QSize desktopSize(gdi->width, gdi->height);
        const QRect dirtyRect = requestedRect.intersected(QRect(QPoint(0, 0), desktopSize));
        if (dirtyRect.isEmpty()) {
            return true;
        }

        constexpr qsizetype bytesPerPixel = 4;
        const qsizetype bytesPerLine = static_cast<qsizetype>(dirtyRect.width()) * bytesPerPixel;
        const qsizetype height = dirtyRect.height();
        if (bytesPerLine <= 0 || bytesPerLine > (std::numeric_limits<int>::max)()
            || height <= 0
            || height > (std::numeric_limits<qsizetype>::max)() / bytesPerLine) {
            error = QStringLiteral("The remote desktop update is too large to display.");
            return false;
        }
        const qsizetype byteCount = bytesPerLine * height;

        DesktopUpdate desktopUpdate;
        desktopUpdate.desktopSize = desktopSize;
        desktopUpdate.dirtyRect = dirtyRect;
        desktopUpdate.bytesPerLine = static_cast<int>(bytesPerLine);
        desktopUpdate.pixels.resize(byteCount);

        const BYTE *source = gdi->primary_buffer
                             + static_cast<qsizetype>(dirtyRect.y()) * gdi->stride
                             + static_cast<qsizetype>(dirtyRect.x()) * bytesPerPixel;
        char *destination = desktopUpdate.pixels.data();
        for (int row = 0; row < dirtyRect.height(); ++row) {
            std::memcpy(destination + static_cast<qsizetype>(row) * bytesPerLine,
                        source + static_cast<qsizetype>(row) * gdi->stride,
                        static_cast<size_t>(bytesPerLine));
        }

        try {
            desktopUpdateHandler(desktopUpdate);
        } catch (...) {
            error = QStringLiteral("The remote desktop update handler failed.");
            return false;
        }
        return true;
    }

    static ClientContext *clientContext(freerdp *instance)
    {
        return instance && instance->context
                   ? reinterpret_cast<ClientContext *>(instance->context)
                   : nullptr;
    }

    static Impl *owner(freerdp *instance)
    {
        ClientContext *context = clientContext(instance);
        return context ? static_cast<Impl *>(context->owner) : nullptr;
    }

    static Impl *owner(void *context)
    {
        ClientContext *client = static_cast<ClientContext *>(context);
        return client ? static_cast<Impl *>(client->owner) : nullptr;
    }

    static Impl *clipboardOwner(CliprdrClientContext *context)
    {
        return context ? static_cast<Impl *>(context->custom) : nullptr;
    }

    static QString fromUtf8(const char *value)
    {
        return value ? QString::fromUtf8(value) : QString();
    }

    static DWORD certificateResult(Impl *implementation, const CertificateInfo &certificate)
    {
        if (!implementation || !implementation->certificateVerifier) {
            if (implementation) {
                implementation->certificateRejected = true;
            }
            return 0;
        }

        const CertificateDecision decision = implementation->certificateVerifier(certificate);
        implementation->certificateRejected = decision == CertificateDecision::Reject;
        return decision == CertificateDecision::TrustOnce ? 2 : 0;
    }

    static DWORD verifyCertificate(freerdp *instance,
                                   const char *host,
                                   UINT16 port,
                                   const char *commonName,
                                   const char *subject,
                                   const char *issuer,
                                   const char *fingerprint,
                                   DWORD flags)
    {
        CertificateInfo certificate;
        certificate.host = fromUtf8(host);
        certificate.port = port;
        certificate.commonName = fromUtf8(commonName);
        certificate.subject = fromUtf8(subject);
        certificate.issuer = fromUtf8(issuer);
        certificate.fingerprint = fromUtf8(fingerprint);
        certificate.hostNameMismatch = (flags & VERIFY_CERT_FLAG_MISMATCH) != 0;
        return certificateResult(owner(instance), certificate);
    }

    static DWORD verifyChangedCertificate(freerdp *instance,
                                          const char *host,
                                          UINT16 port,
                                          const char *commonName,
                                          const char *subject,
                                          const char *issuer,
                                          const char *newFingerprint,
                                          const char *oldSubject,
                                          const char *oldIssuer,
                                          const char *oldFingerprint,
                                          DWORD flags)
    {
        CertificateInfo certificate;
        certificate.host = fromUtf8(host);
        certificate.port = port;
        certificate.commonName = fromUtf8(commonName);
        certificate.subject = fromUtf8(subject);
        certificate.issuer = fromUtf8(issuer);
        certificate.fingerprint = fromUtf8(newFingerprint);
        certificate.oldSubject = fromUtf8(oldSubject);
        certificate.oldIssuer = fromUtf8(oldIssuer);
        certificate.oldFingerprint = fromUtf8(oldFingerprint);
        certificate.hostNameMismatch = (flags & VERIFY_CERT_FLAG_MISMATCH) != 0;
        certificate.changed = true;
        return certificateResult(owner(instance), certificate);
    }

public:

    freerdp *instance = nullptr;
    bool winsockInitialized = false;
    bool connected = false;
    bool certificateRejected = false;
    bool channelEventsSubscribed = false;
    bool clipboardReady = false;
    UINT32 requestedRemoteFormat = 0;
    CliprdrClientContext *clipboardContext = nullptr;
    QString error;
    QString localClipboardText;
    std::function<CertificateDecision(const CertificateInfo &)> certificateVerifier;
    std::function<void(const DesktopUpdate &)> desktopUpdateHandler;
    std::function<void(const QString &)> clipboardTextHandler;
};

RdpClient::RdpClient()
    : d(std::make_unique<Impl>())
{
}

RdpClient::~RdpClient() = default;

bool RdpClient::connectToServer(const ConnectionInfo &info)
{
    return d->connectToServer(info);
}

void RdpClient::disconnect()
{
    d->disconnect();
}

bool RdpClient::processEvents()
{
    return d->processEvents();
}

bool RdpClient::sendKeyboardInput(const RdpKeyboardInput &input)
{
    return d->sendKeyboardInput(input);
}

bool RdpClient::sendPointerInput(const RdpPointerInput &input)
{
    return d->sendPointerInput(input);
}

bool RdpClient::sendClipboardText(const QString &text)
{
    return d->sendClipboardText(text);
}

void RdpClient::setDesktopUpdateHandler(
    std::function<void(const DesktopUpdate &)> handler)
{
    d->setDesktopUpdateHandler(std::move(handler));
}

void RdpClient::setClipboardTextHandler(std::function<void(const QString &)> handler)
{
    d->setClipboardTextHandler(std::move(handler));
}

bool RdpClient::isInitialized() const
{
    return d->isInitialized();
}

bool RdpClient::isConnected() const
{
    return d->connected;
}

QString RdpClient::lastError() const
{
    return d->error;
}

QString RdpClient::libraryVersion()
{
    return QString::fromLatin1(freerdp_get_version_string());
}
