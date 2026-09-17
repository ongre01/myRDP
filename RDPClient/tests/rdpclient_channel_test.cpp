#include "../rdpclient.h"

#include <QtTest>

class RdpClientChannelTest : public QObject
{
    Q_OBJECT

private slots:
    void loadsClipboardChannelThroughReloadCallbackBeforeTransportConnection();
};

void RdpClientChannelTest::loadsClipboardChannelThroughReloadCallbackBeforeTransportConnection()
{
    RdpClient client;
    QVERIFY2(client.isInitialized(), qPrintable(client.lastError()));

    ConnectionInfo connection;
    connection.serverAddress = QStringLiteral("127.0.0.1");
    connection.port = 1;
    connection.username = QStringLiteral("channel-test");
    connection.password = QStringLiteral("unused");
    connection.certificateVerifier = [](const CertificateInfo &) {
        return CertificateDecision::TrustOnce;
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        QVERIFY(!client.connectToServer(connection));
        const QString error = client.lastError();
        QVERIFY2(error.contains(QStringLiteral("could not be reached"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY2(!error.contains(QStringLiteral("cliprdr"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY2(!error.contains(QStringLiteral("clipboard channel"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY(!client.isConnected());
        QCOMPARE(client.clipboardChannelLoadCountForTesting(), attempt + 1);
    }
}

QTEST_APPLESS_MAIN(RdpClientChannelTest)

#include "rdpclient_channel_test.moc"
