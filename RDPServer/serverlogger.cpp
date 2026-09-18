#include "serverlogger.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTextStream>

namespace {
bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}
} // namespace

RdpServerLogger::~RdpServerLogger()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (file.isOpen()) {
        file.flush();
        file.close();
    }
}

bool RdpServerLogger::configure(RdpServerLogLevel level,
                                const QString &filePath,
                                QString *errorMessage)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (file.isOpen()) {
        file.flush();
        file.close();
    }
    file.setFileName(QString());
    minimumLevel = level;

    if (level == RdpServerLogLevel::Off) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    const QString normalizedPath = QFileInfo(filePath).absoluteFilePath();
    if (normalizedPath.isEmpty()) {
        minimumLevel = RdpServerLogLevel::Off;
        return fail(errorMessage, QStringLiteral("The server log file path is empty."));
    }

    file.setFileName(normalizedPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        const QString fileError = file.errorString();
        minimumLevel = RdpServerLogLevel::Off;
        file.setFileName(QString());
        return fail(errorMessage,
                    QStringLiteral("Failed to open the server log file %1: %2")
                        .arg(normalizedPath, fileError));
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool RdpServerLogger::isEnabled(RdpServerLogLevel level) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return isEnabledLocked(level);
}

bool RdpServerLogger::write(RdpServerLogLevel level,
                            const QString &category,
                            const QString &message,
                            QString *errorMessage)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!isEnabledLocked(level)) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }
    if (!file.isOpen()) {
        return fail(errorMessage, QStringLiteral("The server log file is not open."));
    }

    QString singleLineMessage = message;
    singleLineMessage.replace(QLatin1Char('\r'), QLatin1Char(' '));
    singleLineMessage.replace(QLatin1Char('\n'), QLatin1Char(' '));
    QTextStream stream(&file);
    stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
           << " [" << rdpServerLogLevelName(level) << "]"
           << " [" << category << "] " << singleLineMessage << '\n';
    stream.flush();
    if (!file.flush() || file.error() != QFileDevice::NoError) {
        return fail(errorMessage,
                    QStringLiteral("Failed to write the server log file %1: %2")
                        .arg(file.fileName(), file.errorString()));
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

QString RdpServerLogger::filePath() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return file.fileName();
}

bool RdpServerLogger::isEnabledLocked(RdpServerLogLevel level) const
{
    return minimumLevel != RdpServerLogLevel::Off
           && level != RdpServerLogLevel::Off
           && static_cast<int>(level) >= static_cast<int>(minimumLevel);
}
