#include "mainwindow.h"
#include "rdpclient.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QString>

namespace {

void configureOpenSslProviderPath()
{
#if defined(Q_OS_WIN)
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QString legacyProvider =
        QDir(applicationDirectory).filePath(QStringLiteral("legacy.dll"));
    QString providerDirectory;

    if (!qEnvironmentVariableIsEmpty("OPENSSL_MODULES")) {
        providerDirectory = QFile::decodeName(qgetenv("OPENSSL_MODULES"));
    } else if (QFileInfo::exists(legacyProvider)) {
        providerDirectory = applicationDirectory;
        qputenv("OPENSSL_MODULES", QFile::encodeName(providerDirectory));
    }

    if (providerDirectory.isEmpty()) {
        return;
    }

    QLibrary cryptoLibrary(
        QDir(applicationDirectory).filePath(QStringLiteral("libcrypto-3-x64.dll")));
    if (!cryptoLibrary.load()) {
        return;
    }

    using SetProviderSearchPath = int (*)(void *, const char *);
    const auto setProviderSearchPath = reinterpret_cast<SetProviderSearchPath>(
        cryptoLibrary.resolve("OSSL_PROVIDER_set_default_search_path"));
    if (setProviderSearchPath) {
        const QByteArray encodedProviderDirectory = QFile::encodeName(providerDirectory);
        setProviderSearchPath(nullptr, encodedProviderDirectory.constData());
    }
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RDPClient"));
    QApplication::setApplicationVersion(RdpClient::libraryVersion());
    configureOpenSslProviderPath();

    MainWindow w;
    w.show();
    return QApplication::exec();
}
