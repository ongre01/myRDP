#ifndef CLIENTSETTINGS_H
#define CLIENTSETTINGS_H

#include <QString>

struct ConnectionDefaults
{
    QString serverAddress;
    int port = 3389;
    QString username;
    QString domain;
};

class ClientSettings
{
public:
    static QString defaultFilePath();
    static ConnectionDefaults loadConnectionDefaults();
    static ConnectionDefaults loadConnectionDefaults(const QString &filePath);
};

#endif // CLIENTSETTINGS_H
