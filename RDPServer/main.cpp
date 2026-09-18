#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QString>

#include <freerdp/freerdp.h>
#include <freerdp/server/server-common.h>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RDPServer"));
    QApplication::setApplicationDisplayName(QStringLiteral("QtRdp Server"));
    QApplication::setOrganizationName(QStringLiteral("QtRdp"));
    QApplication::setApplicationVersion(QString::fromLatin1(freerdp_get_version_string()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QApplication::translate("main", "QtRdp background RDP server"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption backgroundOption(
        QStringList{QStringLiteral("b"), QStringLiteral("background")},
        QApplication::translate("main", "Start the server without showing the settings window."));
    parser.addOption(backgroundOption);
    parser.process(application);

    const bool startHidden = parser.isSet(backgroundOption);
    MainWindow window(startHidden);
    if (!startHidden) {
        window.show();
    }
    return QApplication::exec();
}
