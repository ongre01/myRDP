#include "rdpserver.h"
#include "rdptestframe_p.h"

#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <freerdp/codec/color.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>

namespace {
struct TestClientContext
{
    rdpContext context;
    std::uint64_t firstFrameHash = 0;
    std::uint64_t lastFrameHash = 0;
    std::uint32_t frameCount = 0;
};

std::uint64_t framebufferHash(const rdpGdi *gdi)
{
    if (!gdi || !gdi->primary_buffer || gdi->stride <= 0 || gdi->height <= 0) {
        return 0;
    }

    constexpr std::uint64_t offsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offsetBasis;
    const std::size_t byteCount = static_cast<std::size_t>(gdi->stride) * gdi->height;
    for (std::size_t index = 0; index < byteCount; ++index) {
        hash ^= gdi->primary_buffer[index];
        hash *= prime;
    }
    return hash;
}

BOOL testClientBeginPaint(rdpContext *context)
{
    if (!context || !context->gdi || !context->gdi->primary
        || !context->gdi->primary->hdc || !context->gdi->primary->hdc->hwnd
        || !context->gdi->primary->hdc->hwnd->invalid) {
        return FALSE;
    }

    HGDI_WND window = context->gdi->primary->hdc->hwnd;
    window->invalid->null = TRUE;
    window->ninvalid = 0;
    return TRUE;
}

BOOL testClientEndPaint(rdpContext *context)
{
    auto *testContext = reinterpret_cast<TestClientContext *>(context);
    const std::uint64_t hash = framebufferHash(context ? context->gdi : nullptr);
    if (!testContext || hash == 0) {
        return FALSE;
    }

    if (testContext->frameCount == 0) {
        testContext->firstFrameHash = hash;
    }
    testContext->lastFrameHash = hash;
    ++testContext->frameCount;
    return TRUE;
}

BOOL testClientPostConnect(freerdp *instance)
{
    if (!instance || !instance->context || !instance->context->update
        || !gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        return FALSE;
    }

    instance->context->update->BeginPaint = testClientBeginPaint;
    instance->context->update->EndPaint = testClientEndPaint;
    return TRUE;
}

void testClientPostDisconnect(freerdp *instance)
{
    if (instance) {
        gdi_free(instance);
    }
}
} // namespace

class RdpServerTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidPort();
    void acceptsAndClosesSession();
    void generatesChangingTestFrames();
    void connectsAndReceivesChangingFrames();
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

void RdpServerTest::generatesChangingTestFrames()
{
    const RdpTestFrame full = RdpTestFrameGenerator::fullFrame(640, 480, 41);
    QVERIFY(full.isValid());
    QCOMPARE(full.x, std::uint16_t(0));
    QCOMPARE(full.y, std::uint16_t(0));
    QCOMPARE(full.width, std::uint16_t(640));
    QCOMPARE(full.height, std::uint16_t(480));
    QCOMPARE(full.pixels.size(), std::size_t(640 * 480 * 4));

    const RdpTestFrame counter41 = RdpTestFrameGenerator::counterFrame(640, 480, 41);
    const RdpTestFrame counter42 = RdpTestFrameGenerator::counterFrame(640, 480, 42);
    QVERIFY(counter41.isValid());
    QVERIFY(counter42.isValid());
    QCOMPARE(counter41.x, counter42.x);
    QCOMPARE(counter41.y, counter42.y);
    QCOMPARE(counter41.width, counter42.width);
    QCOMPARE(counter41.height, counter42.height);
    QVERIFY(counter41.pixels != counter42.pixels);

    const auto firstPixel = full.pixels.cbegin();
    const auto lastPixel = full.pixels.cend();
    QVERIFY(std::adjacent_find(firstPixel, lastPixel, std::not_equal_to<>()) != lastPixel);
}

void RdpServerTest::connectsAndReceivesChangingFrames()
{
    QTcpServer portProbe;
    QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.serverPort();
    portProbe.close();

    RdpServer server;
    QVERIFY2(server.isInitialized(), qPrintable(server.lastError()));

    RdpServerConfiguration configuration;
    configuration.bindAddress = QStringLiteral("127.0.0.1");
    configuration.port = port;
    QVERIFY2(server.start(configuration), qPrintable(server.lastError()));

    freerdp *client = freerdp_new();
    QVERIFY(client);
    bool contextCreated = false;
    bool connected = false;
    const auto cleanup = qScopeGuard([&]() {
        if (connected) {
            (void)freerdp_disconnect(client);
        }
        if (contextCreated) {
            freerdp_context_free(client);
        }
        freerdp_free(client);
        server.stop();
    });

    client->ContextSize = sizeof(TestClientContext);
    client->PostConnect = testClientPostConnect;
    client->PostDisconnect = testClientPostDisconnect;
    QVERIFY(freerdp_context_new(client));
    contextCreated = true;

    rdpSettings *settings = client->context->settings;
    QVERIFY(settings);
    QVERIFY(freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "127.0.0.1"));
    QVERIFY(freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port));
    QVERIFY(freerdp_settings_set_string(settings, FreeRDP_Username, "test"));
    QVERIFY(freerdp_settings_set_string(settings, FreeRDP_Password, "test"));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_Authentication, FALSE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE));
    QVERIFY(freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, TRUE));
    QVERIFY(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 640));
    QVERIFY(freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 480));
    QVERIFY(freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32));

    QVERIFY2(freerdp_connect(client),
             freerdp_get_last_error_name(freerdp_get_last_error(client->context)));
    connected = true;

    auto *testContext = reinterpret_cast<TestClientContext *>(client->context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (testContext->frameCount < 3 && std::chrono::steady_clock::now() < deadline) {
        std::array<HANDLE, 64> handles = {};
        const DWORD handleCount = freerdp_get_event_handles(client->context,
                                                            handles.data(),
                                                            static_cast<DWORD>(handles.size()));
        QVERIFY(handleCount > 0);

        const DWORD waitResult = WaitForMultipleObjects(handleCount,
                                                        handles.data(),
                                                        FALSE,
                                                        100);
        QVERIFY(waitResult == WAIT_TIMEOUT || waitResult < WAIT_OBJECT_0 + handleCount);
        if (waitResult != WAIT_TIMEOUT) {
            QVERIFY2(freerdp_check_event_handles(client->context),
                     freerdp_get_last_error_name(freerdp_get_last_error(client->context)));
        }
    }

    QVERIFY(testContext->frameCount >= 3);
    QVERIFY(testContext->firstFrameHash != 0);
    QVERIFY(testContext->firstFrameHash != testContext->lastFrameHash);
}

QTEST_GUILESS_MAIN(RdpServerTest)

#include "tst_rdpserver.moc"
