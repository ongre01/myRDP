#include "mainwindow.h"

#include <QApplication>
#include <QString>

#include <freerdp/freerdp.h>
#include <freerdp/server/server-common.h>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RDPServer"));
    QApplication::setApplicationVersion(QString::fromLatin1(freerdp_get_version_string()));

    MainWindow w;
    w.show();
    return QApplication::exec();
}
