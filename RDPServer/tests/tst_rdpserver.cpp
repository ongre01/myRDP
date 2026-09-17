#include "rdpserver.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

class RdpServerTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidPort();
    void acceptsAndClosesSession();
};

void RdpServerTest::rejectsInvalidPort()
{
    RdpServer server;
    QVERIFY2(server.isInitialized(), qPrintable(server.lastError()));

    RdpServerConfiguration configuration;
    configuration.bindAddress = QStringLiteral("127.0.0.1");
    configuration.port = 0;

    QVERIFY(!server.start(configuration));
    QVERIFY(!server.lastError().isEmpty());
    QVERIFY(!server.isListening());
}

void RdpServerTest::acceptsAndClosesSession()
{
    QTcpServer portProbe;
    QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.serverPort();
    portProbe.close();

    RdpServer server;
    QVERIFY2(server.isInitialized(), qPrintable(server.lastError()));
    QSignalSpy connectedSpy(&server, &RdpServer::clientConnected);
    QSignalSpy disconnectedSpy(&server, &RdpServer::clientDisconnected);

    RdpServerConfiguration configuration;
    configuration.bindAddress = QStringLiteral("127.0.0.1");
    configuration.port = port;

    QVERIFY2(server.start(configuration), qPrintable(server.lastError()));
    QVERIFY(server.isListening());

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(client.waitForConnected(2000));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QCOMPARE(server.sessionCount(), qsizetype(1));

    QCOMPARE(client.write("RDP probe"), qint64(9));
    QVERIFY(client.waitForBytesWritten(2000));
    client.disconnectFromHost();
    if (client.state() != QAbstractSocket::UnconnectedState) {
        QVERIFY(client.waitForDisconnected(2000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);

    server.stop();
    QVERIFY(!server.isListening());

    QVERIFY2(server.start(configuration), qPrintable(server.lastError()));
    QVERIFY(server.isListening());
    server.stop();
    QVERIFY(!server.isListening());
}

QTEST_GUILESS_MAIN(RdpServerTest)

#include "tst_rdpserver.moc"
