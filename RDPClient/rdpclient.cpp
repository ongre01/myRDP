#include "rdpclient.h"

#include <QByteArray>

#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>

namespace {

struct ClientContext
{
    rdpContext context;
    void *owner;
};

BOOL preConnect(freerdp *instance)
{
    return instance && instance->context ? TRUE : FALSE;
}

BOOL postConnect(freerdp *instance)
{
    return instance && instance->context ? TRUE : FALSE;
}

QString connectionError(const rdpContext *context)
{
    if (!context) {
        return QStringLiteral("FreeRDP connection failed without an initialized context.");
    }

    const UINT32 errorCode = freerdp_get_last_error(context);
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
        instance = freerdp_new();
        if (!instance) {
            error = QStringLiteral("FreeRDP instance initialization failed.");
            return;
        }

        instance->ContextSize = sizeof(ClientContext);
        instance->PreConnect = preConnect;
        instance->PostConnect = postConnect;
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
            error = certificateRejected
                        ? QStringLiteral("The RDP server certificate was rejected.")
                        : connectionError(instance->context);
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

    bool isInitialized() const
    {
        return instance && instance->context;
    }

private:
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
    bool connected = false;
    bool certificateRejected = false;
    QString error;
    std::function<CertificateDecision(const CertificateInfo &)> certificateVerifier;
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
