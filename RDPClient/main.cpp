#include "mainwindow.h"

#include <QApplication>
#include <QString>

#include <freerdp/client.h>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RDPClient"));
    QApplication::setApplicationVersion(QString::fromLatin1(freerdp_get_version_string()));

    MainWindow w;
    w.show();
    return QApplication::exec();
}
