#include "clientsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace {

constexpr int defaultRdpPort = 3389;
constexpr int minimumPort = 1;
constexpr int maximumPort = 65535;

} // namespace

QString ClientSettings::defaultFilePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("RDPClient.ini"));
}

ConnectionDefaults ClientSettings::loadConnectionDefaults()
{
    return loadConnectionDefaults(defaultFilePath());
}

ConnectionDefaults ClientSettings::loadConnectionDefaults(const QString &filePath)
{
    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Connection"));

    ConnectionDefaults defaults;
    defaults.serverAddress = settings.value(QStringLiteral("ServerAddress")).toString().trimmed();
    defaults.username = settings.value(QStringLiteral("UserName")).toString();
    defaults.domain = settings.value(QStringLiteral("Domain")).toString();

    bool portIsNumber = false;
    const int configuredPort = settings.value(QStringLiteral("Port"), defaultRdpPort)
                                   .toString()
                                   .toInt(&portIsNumber);
    if (portIsNumber && configuredPort >= minimumPort && configuredPort <= maximumPort) {
        defaults.port = configuredPort;
    }

    settings.endGroup();
    return defaults;
}
