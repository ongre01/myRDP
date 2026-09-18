#include "autostartmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace {
constexpr auto runRegistryPath =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto runRegistryValueName = "QtRdpServer";

QString translated(const char *text)
{
    return QCoreApplication::translate("AutoStartManager", text);
}
} // namespace

AutoStartManager::AutoStartManager(QString applicationFilePath)
    : applicationFilePath(applicationFilePath.trimmed())
{
    if (this->applicationFilePath.isEmpty()) {
        this->applicationFilePath = QCoreApplication::applicationFilePath();
    }
}

bool AutoStartManager::isSupported() const
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

bool AutoStartManager::isEnabled() const
{
#if defined(Q_OS_WIN)
    QSettings settings(QString::fromLatin1(runRegistryPath), QSettings::NativeFormat);
    const QString registeredCommand =
        settings.value(QString::fromLatin1(runRegistryValueName)).toString();
    return registeredCommand.compare(startupCommand(applicationFilePath),
                                     Qt::CaseInsensitive)
           == 0;
#else
    return false;
#endif
}

bool AutoStartManager::setEnabled(bool enabled, QString *errorMessage) const
{
    if (errorMessage) {
        errorMessage->clear();
    }

#if defined(Q_OS_WIN)
    const QString command = startupCommand(applicationFilePath);
    if (enabled && command.isEmpty()) {
        if (errorMessage) {
            *errorMessage = translated("The application path is unavailable.");
        }
        return false;
    }

    QSettings settings(QString::fromLatin1(runRegistryPath), QSettings::NativeFormat);
    if (enabled) {
        settings.setValue(QString::fromLatin1(runRegistryValueName), command);
    } else {
        settings.remove(QString::fromLatin1(runRegistryValueName));
    }
    settings.sync();

    if (settings.status() == QSettings::NoError) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = settings.status() == QSettings::AccessError
                            ? translated("Windows denied access to the startup setting.")
                            : translated("The Windows startup setting could not be updated.");
    }
    return false;
#else
    Q_UNUSED(enabled)
    if (errorMessage) {
        *errorMessage = translated("Automatic startup is only supported on Windows.");
    }
    return false;
#endif
}

QString AutoStartManager::startupCommand(const QString &applicationFilePath)
{
    const QString trimmedPath = applicationFilePath.trimmed();
    if (trimmedPath.isEmpty()) {
        return QString();
    }
    return QStringLiteral("\"%1\" --background")
        .arg(QDir::toNativeSeparators(trimmedPath));
}
