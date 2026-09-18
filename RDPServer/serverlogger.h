#ifndef SERVERLOGGER_H
#define SERVERLOGGER_H

#include "serverconfiguration.h"

#include <QFile>
#include <QString>

#include <mutex>

class RdpServerLogger final
{
public:
    RdpServerLogger() = default;
    ~RdpServerLogger();

    RdpServerLogger(const RdpServerLogger &) = delete;
    RdpServerLogger &operator=(const RdpServerLogger &) = delete;

    bool configure(RdpServerLogLevel level,
                   const QString &filePath,
                   QString *errorMessage = nullptr);
    bool isEnabled(RdpServerLogLevel level) const;
    bool write(RdpServerLogLevel level,
               const QString &category,
               const QString &message,
               QString *errorMessage = nullptr);

    QString filePath() const;

private:
    bool isEnabledLocked(RdpServerLogLevel level) const;

    mutable std::mutex mutex;
    QFile file;
    RdpServerLogLevel minimumLevel = RdpServerLogLevel::Off;
};

#endif // SERVERLOGGER_H
