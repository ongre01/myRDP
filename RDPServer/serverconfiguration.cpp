#include "serverconfiguration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QSettings>
#include <QStringList>

#include <limits>

namespace {
constexpr quint32 minimumPort = 1;
constexpr quint32 maximumPort = 65535;
constexpr quint32 maximumSupportedClients = 1024;

QString settingString(const QSettings &settings,
                      const QStringList &keys,
                      const QString &defaultValue = {})
{
    for (const QString &key : keys) {
        if (settings.contains(key)) {
            return settings.value(key).toString().trimmed();
        }
    }
    return defaultValue;
}

bool parseUnsigned(const QString &value, quint32 *result)
{
    bool parsed = false;
    const qulonglong number = value.toULongLong(&parsed, 10);
    if (!parsed || number > (std::numeric_limits<quint32>::max)()) {
        return false;
    }
    if (result) {
        *result = static_cast<quint32>(number);
    }
    return true;
}

QString resolvePath(const QDir &baseDirectory, const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(path)) {
        return QDir::cleanPath(path);
    }
    return QDir::cleanPath(baseDirectory.absoluteFilePath(path));
}

bool parseAuthentication(const QString &value, RdpServerAuthentication *authentication)
{
    if (value.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("0")) {
        *authentication = RdpServerAuthentication::Disabled;
        return true;
    }
    if (value.compare(QStringLiteral("nla"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("enabled"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || value == QStringLiteral("1")) {
        *authentication = RdpServerAuthentication::Nla;
        return true;
    }
    return false;
}

bool parseLogLevel(const QString &value, RdpServerLogLevel *level)
{
    const struct {
        const char *name;
        RdpServerLogLevel level;
    } levels[] = {
        {"debug", RdpServerLogLevel::Debug},
        {"info", RdpServerLogLevel::Info},
        {"warning", RdpServerLogLevel::Warning},
        {"error", RdpServerLogLevel::Error},
        {"off", RdpServerLogLevel::Off},
    };

    for (const auto &entry : levels) {
        if (value.compare(QString::fromLatin1(entry.name), Qt::CaseInsensitive) == 0) {
            *level = entry.level;
            return true;
        }
    }
    return false;
}

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}
} // namespace

bool RdpServerConfiguration::validate(QString *errorMessage) const
{
    const QString address = bindAddress.trimmed();
    QHostAddress parsedAddress;
    if (!address.isEmpty() && !parsedAddress.setAddress(address)) {
        return fail(errorMessage,
                    QStringLiteral("ListenAddress must be an IPv4 or IPv6 address."));
    }
    if (port < minimumPort || port > maximumPort) {
        return fail(errorMessage,
                    QStringLiteral("RdpPort must be between 1 and 65535."));
    }
    if (maximumClientCount == 0 || maximumClientCount > maximumSupportedClients) {
        return fail(errorMessage,
                    QStringLiteral("MaximumClientCount must be between 1 and 1024."));
    }
    if (captureMode != RdpServerCaptureMode::Desktop) {
        return fail(errorMessage, QStringLiteral("Capture must be Desktop."));
    }

    const bool hasCertificate = !certificateFile.trimmed().isEmpty();
    const bool hasPrivateKey = !privateKeyFile.trimmed().isEmpty();
    if (hasCertificate != hasPrivateKey) {
        return fail(errorMessage,
                    QStringLiteral("Certificate and PrivateKey must be configured together."));
    }
    if (hasCertificate) {
        const QFileInfo certificate(certificateFile);
        const QFileInfo privateKey(privateKeyFile);
        if (!certificate.isFile() || !certificate.isReadable()) {
            return fail(errorMessage,
                        QStringLiteral("Certificate is not a readable file: %1")
                            .arg(certificateFile));
        }
        if (!privateKey.isFile() || !privateKey.isReadable()) {
            return fail(errorMessage,
                        QStringLiteral("PrivateKey is not a readable file: %1")
                            .arg(privateKeyFile));
        }
    }

    if (!ntlmSamFile.trimmed().isEmpty()) {
        const QFileInfo samFile(ntlmSamFile);
        if (!samFile.isFile() || !samFile.isReadable()) {
            return fail(errorMessage,
                        QStringLiteral("NtlmSamFile is not a readable file: %1")
                            .arg(ntlmSamFile));
        }
    }

    if (logLevel != RdpServerLogLevel::Off && logFilePath.trimmed().isEmpty()) {
        return fail(errorMessage,
                    QStringLiteral("LogFile must not be empty when logging is enabled."));
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

QString RdpServerSettings::defaultFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("RDPServer.ini"));
}

QString RdpServerSettings::defaultLogFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("RDPServer.log"));
}

bool RdpServerSettings::loadDefault(RdpServerConfiguration *configuration,
                                    QString *errorMessage)
{
    const QString filePath = defaultFilePath();
    if (!QFileInfo::exists(filePath)) {
        if (!configuration) {
            return fail(errorMessage, QStringLiteral("The configuration output is null."));
        }
        RdpServerConfiguration defaults;
        defaults.logFilePath = defaultLogFilePath();
        *configuration = defaults;
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }
    return load(filePath, configuration, errorMessage);
}

bool RdpServerSettings::load(const QString &filePath,
                             RdpServerConfiguration *configuration,
                             QString *errorMessage)
{
    if (!configuration) {
        return fail(errorMessage, QStringLiteral("The configuration output is null."));
    }

    const QFileInfo configurationFile(filePath);
    if (!configurationFile.isFile() || !configurationFile.isReadable()) {
        return fail(errorMessage,
                    QStringLiteral("The server configuration file is not readable: %1")
                        .arg(filePath));
    }

    QSettings settings(configurationFile.absoluteFilePath(), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Server"));

    RdpServerConfiguration loaded;
    loaded.bindAddress = settingString(settings,
                                       {QStringLiteral("ListenAddress"),
                                        QStringLiteral("Listen Address")},
                                       loaded.bindAddress);

    const QString portText = settingString(settings,
                                           {QStringLiteral("RdpPort"),
                                            QStringLiteral("RDP Port"),
                                            QStringLiteral("Port")},
                                           QString::number(loaded.port));
    if (!parseUnsigned(portText, &loaded.port)) {
        return fail(errorMessage, QStringLiteral("RdpPort must be an integer."));
    }

    const QString authenticationText = settingString(
        settings,
        {QStringLiteral("Authentication")},
        QStringLiteral("Disabled"));
    if (!parseAuthentication(authenticationText, &loaded.authentication)) {
        return fail(errorMessage,
                    QStringLiteral("Authentication must be Disabled or Nla."));
    }

    const QDir baseDirectory = configurationFile.absoluteDir();
    loaded.certificateFile = resolvePath(
        baseDirectory,
        settingString(settings,
                      {QStringLiteral("Certificate"), QStringLiteral("CertificateFile")}));
    loaded.privateKeyFile = resolvePath(
        baseDirectory,
        settingString(settings,
                      {QStringLiteral("PrivateKey"), QStringLiteral("PrivateKeyFile")}));
    loaded.ntlmSamFile = resolvePath(baseDirectory,
                                     settingString(settings,
                                                   {QStringLiteral("NtlmSamFile")}));

    const QString captureText = settingString(settings,
                                              {QStringLiteral("Capture")},
                                              QStringLiteral("Desktop"));
    if (captureText.compare(QStringLiteral("Desktop"), Qt::CaseInsensitive) != 0) {
        return fail(errorMessage, QStringLiteral("Capture must be Desktop."));
    }
    loaded.captureMode = RdpServerCaptureMode::Desktop;

    const QString maximumClientText = settingString(
        settings,
        {QStringLiteral("MaximumClientCount"), QStringLiteral("Maximum Client Count")},
        QString::number(loaded.maximumClientCount));
    if (!parseUnsigned(maximumClientText, &loaded.maximumClientCount)) {
        return fail(errorMessage,
                    QStringLiteral("MaximumClientCount must be an integer."));
    }

    const QString logLevelText = settingString(settings,
                                               {QStringLiteral("LogLevel"),
                                                QStringLiteral("Log Level")},
                                               QStringLiteral("Info"));
    if (!parseLogLevel(logLevelText, &loaded.logLevel)) {
        return fail(errorMessage,
                    QStringLiteral("LogLevel must be Debug, Info, Warning, Error, or Off."));
    }
    loaded.logFilePath = resolvePath(
        baseDirectory,
        settingString(settings,
                      {QStringLiteral("LogFile")},
                      QStringLiteral("RDPServer.log")));

    settings.endGroup();
    if (settings.status() != QSettings::NoError) {
        return fail(errorMessage,
                    QStringLiteral("Failed to read the server configuration file: %1")
                        .arg(filePath));
    }

    QString validationError;
    if (!loaded.validate(&validationError)) {
        return fail(errorMessage,
                    QStringLiteral("Invalid server configuration: %1").arg(validationError));
    }

    *configuration = loaded;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

QString rdpServerLogLevelName(RdpServerLogLevel level)
{
    switch (level) {
    case RdpServerLogLevel::Debug:
        return QStringLiteral("DEBUG");
    case RdpServerLogLevel::Info:
        return QStringLiteral("INFO");
    case RdpServerLogLevel::Warning:
        return QStringLiteral("WARNING");
    case RdpServerLogLevel::Error:
        return QStringLiteral("ERROR");
    case RdpServerLogLevel::Off:
        return QStringLiteral("OFF");
    }
    return QStringLiteral("UNKNOWN");
}
