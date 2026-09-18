#ifndef AUTOSTARTMANAGER_H
#define AUTOSTARTMANAGER_H

#include <QString>

class AutoStartManager
{
public:
    explicit AutoStartManager(QString applicationFilePath = QString());

    bool isSupported() const;
    bool isEnabled() const;
    bool setEnabled(bool enabled, QString *errorMessage = nullptr) const;

    static QString startupCommand(const QString &applicationFilePath);

private:
    QString applicationFilePath;
};

#endif // AUTOSTARTMANAGER_H
