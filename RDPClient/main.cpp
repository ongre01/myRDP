#include "mainwindow.h"
#include "rdpclient.h"

#include <QApplication>
#include <QString>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RDPClient"));
    QApplication::setApplicationVersion(RdpClient::libraryVersion());

    MainWindow w;
    w.show();
    return QApplication::exec();
}
