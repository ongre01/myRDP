#include "rdpclient.h"

#include <QByteArray>

#include <cstring>
#include <limits>
#include <utility>

#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/settings.h>
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
            || !freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE)) {
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

    void setDesktopUpdateHandler(std::function<void(const DesktopUpdate &)> handler)
    {
        desktopUpdateHandler = std::move(handler);
    }

    bool isInitialized() const
    {
        return instance && instance->context;
    }

private:
    static BOOL preConnect(freerdp *instance)
    {
        if (!instance || !instance->context || !instance->context->settings) {
            return FALSE;
        }

        rdpSettings *settings = instance->context->settings;
        return freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE)
                   && freerdp_settings_set_bool(settings, FreeRDP_DesktopResize, TRUE);
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
        gdi_free(instance);
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
    QString error;
    std::function<CertificateDecision(const CertificateInfo &)> certificateVerifier;
    std::function<void(const DesktopUpdate &)> desktopUpdateHandler;
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

void RdpClient::setDesktopUpdateHandler(
    std::function<void(const DesktopUpdate &)> handler)
{
    d->setDesktopUpdateHandler(std::move(handler));
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
