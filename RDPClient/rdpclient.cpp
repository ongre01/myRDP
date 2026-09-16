#include "rdpclient.h"

#include <QByteArray>

#include <freerdp/freerdp.h>
#include <freerdp/settings.h>

namespace {

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

    return QStringLiteral("%1 (0x%2): %3")
        .arg(errorName ? QString::fromLatin1(errorName) : QStringLiteral("FREERDP_ERROR_UNKNOWN"),
             hexadecimalCode,
             errorDescription ? QString::fromLatin1(errorDescription)
                              : QStringLiteral("No error description is available."));
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

        instance->PreConnect = preConnect;
        instance->PostConnect = postConnect;

        if (!freerdp_context_new(instance)) {
            error = QStringLiteral("FreeRDP context initialization failed.");
            freerdp_free(instance);
            instance = nullptr;
        }
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

        rdpSettings *settings = instance->context->settings;
        const QByteArray encodedAddress = serverAddress.toUtf8();

        if (!freerdp_settings_set_string(settings,
                                         FreeRDP_ServerHostname,
                                         encodedAddress.constData())
            || !freerdp_settings_set_uint32(settings,
                                            FreeRDP_ServerPort,
                                            static_cast<UINT32>(info.port))) {
            error = QStringLiteral("FreeRDP connection settings could not be configured.");
            return false;
        }

        error.clear();
        if (!freerdp_connect(instance)) {
            error = connectionError(instance->context);
            return false;
        }

        connected = true;
        return true;
    }

    void disconnect()
    {
        if (!connected || !isInitialized()) {
            return;
        }

        if (!freerdp_disconnect(instance)) {
            error = connectionError(instance->context);
        }
        connected = false;
    }

    bool isInitialized() const
    {
        return instance && instance->context;
    }

    freerdp *instance = nullptr;
    bool connected = false;
    QString error;
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
