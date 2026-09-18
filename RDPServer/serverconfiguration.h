#ifndef SERVERCONFIGURATION_H
#define SERVERCONFIGURATION_H

#include <QMetaType>
#include <QString>
#include <QtTypes>

enum class RdpServerAuthentication
{
    Disabled,
    Nla
};

enum class RdpServerCaptureMode
{
    Desktop
};

enum class RdpServerLogLevel
{
    Debug = 0,
    Info,
    Warning,
    Error,
    Off
};

Q_DECLARE_METATYPE(RdpServerLogLevel)

struct RdpServerConfiguration
{
    QString bindAddress = QStringLiteral("0.0.0.0");
    quint32 port = 3389;
    RdpServerAuthentication authentication = RdpServerAuthentication::Disabled;
    QString certificateFile;
    QString privateKeyFile;
    QString ntlmSamFile;
    RdpServerCaptureMode captureMode = RdpServerCaptureMode::Desktop;
    quint32 maximumClientCount = 1;
    RdpServerLogLevel logLevel = RdpServerLogLevel::Info;
    QString logFilePath;

    bool validate(QString *errorMessage = nullptr) const;
};

class RdpServerSettings
{
public:
    static QString defaultFilePath();
    static QString defaultLogFilePath();

    static bool loadDefault(RdpServerConfiguration *configuration,
                            QString *errorMessage = nullptr);
    static bool load(const QString &filePath,
                     RdpServerConfiguration *configuration,
                     QString *errorMessage = nullptr);
};

QString rdpServerLogLevelName(RdpServerLogLevel level);

#endif // SERVERCONFIGURATION_H
