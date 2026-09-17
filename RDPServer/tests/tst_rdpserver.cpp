#include "desktopcapture.h"
#include "desktopframebuffer_p.h"
#include "inputcontroller.h"
#include "rdpinputhandler_p.h"
#include "rdpserver.h"

#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#if defined(Q_OS_WIN)
#include "windowsinputcontroller_p.h"
#endif

#include <freerdp/codec/color.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace {
enum class RecordedInputType
{
    ScanCode,
    Unicode,
    PointerMove,
    RelativePointerMove,
    MouseButton,
    Wheel
};

struct RecordedInput
{
    RecordedInputType type = RecordedInputType::ScanCode;
    std::int32_t first = 0;
    std::int32_t second = 0;
    bool releasedOrPressed = false;
    bool extended = false;
    InputMouseButton button = InputMouseButton::Left;
    InputWheelAxis axis = InputWheelAxis::Vertical;
};

class RecordingInputController final : public InputController
{
public:
    bool sendScanCode(std::uint16_t scanCode,
                      bool released,
                      bool extended,
                      QString *) override
    {
        if (failNext) {
            failNext = false;
            return false;
        }
        records.push_back({RecordedInputType::ScanCode,
                           scanCode,
                           0,
                           released,
                           extended});
        return true;
    }

    bool sendUnicodeCodeUnit(std::uint16_t codeUnit,
                             bool released,
                             QString *) override
    {
        records.push_back({RecordedInputType::Unicode, codeUnit, 0, released});
        return true;
    }

    bool movePointer(std::uint16_t x,
                     std::uint16_t y,
                     QString *) override
    {
        records.push_back({RecordedInputType::PointerMove, x, y});
        return true;
    }

    bool movePointerRelative(std::int16_t deltaX,
                             std::int16_t deltaY,
                             QString *) override
    {
        records.push_back({RecordedInputType::RelativePointerMove, deltaX, deltaY});
        return true;
    }

    bool setMouseButton(InputMouseButton button,
                        bool pressed,
                        QString *) override
    {
        RecordedInput record;
        record.type = RecordedInputType::MouseButton;
        record.releasedOrPressed = pressed;
        record.button = button;
        records.push_back(record);
        return true;
    }

    bool scroll(InputWheelAxis axis,
                std::int16_t delta,
                QString *) override
    {
        RecordedInput record;
        record.type = RecordedInputType::Wheel;
        record.first = delta;
        record.axis = axis;
        records.push_back(record);
        return true;
    }

    std::vector<RecordedInput> records;
    bool failNext = false;
};

struct TestClientContext
{
    rdpContext context;
    std::uint64_t firstFrameHash = 0;
    std::uint64_t lastFrameHash = 0;
    std::uint32_t frameCount = 0;
};

struct TestConnectionResult
{
    bool connected = false;
    bool tlsNegotiated = false;
    std::uint64_t firstFrameHash = 0;
    std::uint64_t lastFrameHash = 0;
    std::uint32_t frameCount = 0;
    QString error;
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

TestConnectionResult connectAndReceiveFrames(quint16 port)
{
    TestConnectionResult result;
    freerdp *client = freerdp_new();
    if (!client) {
        result.error = QStringLiteral("Failed to create the FreeRDP test client.");
        return result;
    }

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
    });

    client->ContextSize = sizeof(TestClientContext);
    client->PostConnect = testClientPostConnect;
    client->PostDisconnect = testClientPostDisconnect;
    if (!freerdp_context_new(client)) {
        result.error = QStringLiteral("Failed to create the FreeRDP test client context.");
        return result;
    }
    contextCreated = true;

    rdpSettings *settings = client->context->settings;
    if (!settings
        || !freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "127.0.0.1")
        || !freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port)
        || !freerdp_settings_set_string(settings, FreeRDP_Username, "test")
        || !freerdp_settings_set_string(settings, FreeRDP_Password, "test")
        || !freerdp_settings_set_bool(settings, FreeRDP_Authentication, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE)
        || !freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE)
        || !freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, TRUE)
        || !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 640)
        || !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 480)
        || !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32)) {
        result.error = QStringLiteral("Failed to configure the FreeRDP TLS test client.");
        return result;
    }

    if (!freerdp_connect(client)) {
        const UINT32 error = freerdp_get_last_error(client->context);
        result.error = QString::fromUtf8(freerdp_get_last_error_name(error));
        if (result.error.isEmpty()) {
            result.error = QStringLiteral("FreeRDP TLS connection failed with error %1.").arg(error);
        }
        return result;
    }
    connected = true;
    result.connected = true;
    result.tlsNegotiated = freerdp_settings_get_bool(settings, FreeRDP_TlsSecurity)
                           && !freerdp_settings_get_bool(settings, FreeRDP_RdpSecurity)
                           && !freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity);

    auto *testContext = reinterpret_cast<TestClientContext *>(client->context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (testContext->frameCount < 1 && std::chrono::steady_clock::now() < deadline) {
        std::array<HANDLE, 64> handles = {};
        const DWORD handleCount = freerdp_get_event_handles(client->context,
                                                            handles.data(),
                                                            static_cast<DWORD>(handles.size()));
        if (handleCount == 0) {
            result.error = QStringLiteral("FreeRDP returned no client event handles.");
            break;
        }

        const DWORD waitResult = WaitForMultipleObjects(handleCount,
                                                        handles.data(),
                                                        FALSE,
                                                        100);
        if (waitResult == WAIT_FAILED
            || (waitResult != WAIT_TIMEOUT && waitResult >= WAIT_OBJECT_0 + handleCount)) {
            result.error = QStringLiteral("Waiting for a FreeRDP client event failed.");
            break;
        }
        if (waitResult != WAIT_TIMEOUT && !freerdp_check_event_handles(client->context)) {
            const UINT32 error = freerdp_get_last_error(client->context);
            result.error = QString::fromUtf8(freerdp_get_last_error_name(error));
            if (result.error.isEmpty()) {
                result.error = QStringLiteral("FreeRDP event processing failed with error %1.")
                                   .arg(error);
            }
            break;
        }
    }

    result.frameCount = testContext->frameCount;
    result.firstFrameHash = testContext->firstFrameHash;
    result.lastFrameHash = testContext->lastFrameHash;
    if (result.frameCount < 1 && result.error.isEmpty()) {
        result.error = QStringLiteral("Timed out after receiving %1 desktop frames.")
                           .arg(result.frameCount);
    }
    return result;
}
} // namespace

class RdpServerTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidPort();
    void acceptsAndClosesSession();
    void tracksDesktopFrameChanges();
    void capturesWindowsDesktop();
    void routesRdpInputEvents();
    void convertsWindowsInputEvents();
    void connectsWithTlsAndReconnects();
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

void RdpServerTest::tracksDesktopFrameChanges()
{
    DesktopFrameBuffer frameBuffer;
    const DesktopSize initialSize = {32, 32};
    const std::uint32_t initialStride = initialSize.width * desktopCaptureBytesPerPixel;
    std::vector<std::uint8_t> pixels(initialStride * initialSize.height, 0x20);

    const DesktopCaptureResult initial = frameBuffer.update(initialSize,
                                                             initialStride,
                                                             pixels,
                                                             false);
    QCOMPARE(initial.status, DesktopCaptureStatus::FrameReady);
    QVERIFY(initial.frame.isValid());
    QCOMPARE(initial.frame.x, std::uint32_t(0));
    QCOMPARE(initial.frame.y, std::uint32_t(0));
    QCOMPARE(initial.frame.width, initialSize.width);
    QCOMPARE(initial.frame.height, initialSize.height);
    QVERIFY(!initial.frame.desktopSizeChanged);

    const DesktopCaptureResult unchanged = frameBuffer.update(initialSize,
                                                               initialStride,
                                                               pixels,
                                                               false);
    QCOMPARE(unchanged.status, DesktopCaptureStatus::NoChanges);

    const std::size_t changedPixel = static_cast<std::size_t>(5) * initialStride
                                     + static_cast<std::size_t>(20)
                                           * desktopCaptureBytesPerPixel;
    pixels[changedPixel] = 0xF0;
    const DesktopCaptureResult changed = frameBuffer.update(initialSize,
                                                             initialStride,
                                                             pixels,
                                                             false);
    QCOMPARE(changed.status, DesktopCaptureStatus::FrameReady);
    QVERIFY(changed.frame.isValid());
    QCOMPARE(changed.frame.x, std::uint32_t(16));
    QCOMPARE(changed.frame.y, std::uint32_t(0));
    QCOMPARE(changed.frame.width, std::uint32_t(16));
    QCOMPARE(changed.frame.height, std::uint32_t(16));

    const DesktopSize resizedSize = {48, 16};
    const std::uint32_t resizedStride = resizedSize.width * desktopCaptureBytesPerPixel;
    pixels.assign(resizedStride * resizedSize.height, 0x40);
    const DesktopCaptureResult resized = frameBuffer.update(resizedSize,
                                                             resizedStride,
                                                             pixels,
                                                             false);
    QCOMPARE(resized.status, DesktopCaptureStatus::FrameReady);
    QVERIFY(resized.frame.isValid());
    QVERIFY(resized.frame.desktopSizeChanged);
    QCOMPARE(resized.frame.width, resizedSize.width);
    QCOMPARE(resized.frame.height, resizedSize.height);

    const DesktopCaptureResult invalid = frameBuffer.update(resizedSize,
                                                             resizedStride,
                                                             {},
                                                             false);
    QCOMPARE(invalid.status, DesktopCaptureStatus::Error);
    QVERIFY(!invalid.errorMessage.isEmpty());
}

void RdpServerTest::capturesWindowsDesktop()
{
#if defined(Q_OS_WIN)
    std::unique_ptr<DesktopCapture> capture = createDesktopCapture();
    QVERIFY(capture);

    QString errorMessage;
    const DesktopSize size = capture->desktopSize(&errorMessage);
    QVERIFY2(size.isValid(), qPrintable(errorMessage));

    const DesktopCaptureResult first = capture->capture(true);
    QCOMPARE(first.status, DesktopCaptureStatus::FrameReady);
    QVERIFY2(first.frame.isValid(), qPrintable(first.errorMessage));
    QVERIFY(first.frame.desktopSize == size);

    const DesktopCaptureResult second = capture->capture(false);
    QVERIFY2(second.status != DesktopCaptureStatus::Error,
             qPrintable(second.errorMessage));
    if (second.status == DesktopCaptureStatus::FrameReady) {
        QVERIFY(second.frame.isValid());
    }
#else
    QSKIP("Windows desktop capture is only available on Windows.");
#endif
}

void RdpServerTest::routesRdpInputEvents()
{
    RecordingInputController controller;
    RdpInputHandler handler(controller);
    QString errorMessage;

    QVERIFY(handler.keyboardEvent(KBD_FLAGS_EXTENDED, 0x1D, &errorMessage));
    QVERIFY(handler.keyboardEvent(KBD_FLAGS_RELEASE, 0x1D, &errorMessage));
    QVERIFY(handler.unicodeKeyboardEvent(KBD_FLAGS_RELEASE, 0xAC00, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_MOVE, 320, 240, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN, 320, 240, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_BUTTON2, 320, 240, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_BUTTON3 | PTR_FLAGS_DOWN, 320, 240, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_WHEEL | 120, 0, 0, &errorMessage));
    QVERIFY(handler.mouseEvent(PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0xD0,
                               0,
                               0,
                               &errorMessage));
    QVERIFY(handler.relativeMouseEvent(PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN,
                                       -7,
                                       9,
                                       &errorMessage));
    QVERIFY(handler.extendedMouseEvent(PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_DOWN,
                                       0,
                                       0,
                                       &errorMessage));
    QVERIFY(handler.extendedMouseEvent(PTR_XFLAGS_BUTTON2, 0, 0, &errorMessage));

    QCOMPARE(controller.records.size(), std::size_t(13));

    const RecordedInput &scanDown = controller.records[0];
    QVERIFY(scanDown.type == RecordedInputType::ScanCode);
    QCOMPARE(scanDown.first, std::int32_t(0x1D));
    QVERIFY(!scanDown.releasedOrPressed);
    QVERIFY(scanDown.extended);

    const RecordedInput &scanUp = controller.records[1];
    QVERIFY(scanUp.type == RecordedInputType::ScanCode);
    QVERIFY(scanUp.releasedOrPressed);
    QVERIFY(!scanUp.extended);

    const RecordedInput &unicode = controller.records[2];
    QVERIFY(unicode.type == RecordedInputType::Unicode);
    QCOMPARE(unicode.first, std::int32_t(0xAC00));
    QVERIFY(unicode.releasedOrPressed);

    const RecordedInput &move = controller.records[3];
    QVERIFY(move.type == RecordedInputType::PointerMove);
    QCOMPARE(move.first, std::int32_t(320));
    QCOMPARE(move.second, std::int32_t(240));

    QVERIFY(controller.records[4].button == InputMouseButton::Left);
    QVERIFY(controller.records[4].releasedOrPressed);
    QVERIFY(controller.records[5].button == InputMouseButton::Right);
    QVERIFY(!controller.records[5].releasedOrPressed);
    QVERIFY(controller.records[6].button == InputMouseButton::Middle);
    QVERIFY(controller.records[6].releasedOrPressed);

    QVERIFY(controller.records[7].type == RecordedInputType::Wheel);
    QVERIFY(controller.records[7].axis == InputWheelAxis::Vertical);
    QCOMPARE(controller.records[7].first, std::int32_t(120));
    QVERIFY(controller.records[8].axis == InputWheelAxis::Horizontal);
    QCOMPARE(controller.records[8].first, std::int32_t(-48));

    QVERIFY(controller.records[9].type == RecordedInputType::RelativePointerMove);
    QCOMPARE(controller.records[9].first, std::int32_t(-7));
    QCOMPARE(controller.records[9].second, std::int32_t(9));
    QVERIFY(controller.records[10].button == InputMouseButton::Left);
    QVERIFY(controller.records[11].button == InputMouseButton::X1);
    QVERIFY(controller.records[11].releasedOrPressed);
    QVERIFY(controller.records[12].button == InputMouseButton::X2);
    QVERIFY(!controller.records[12].releasedOrPressed);

    controller.failNext = true;
    errorMessage.clear();
    QVERIFY(!handler.keyboardEvent(0, 0x20, &errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void RdpServerTest::convertsWindowsInputEvents()
{
#if defined(Q_OS_WIN)
    std::vector<INPUT> injectedInputs;
    WindowsInputController controller(
        [&injectedInputs](UINT count, const INPUT *inputs, int size) {
            if (count != 1 || !inputs || size != sizeof(INPUT)) {
                return UINT(0);
            }
            injectedInputs.push_back(inputs[0]);
            return count;
        },
        [](int index) {
            if (index == SM_CXVIRTUALSCREEN) {
                return 1920;
            }
            if (index == SM_CYVIRTUALSCREEN) {
                return 1080;
            }
            return 0;
        });

    QString errorMessage;
    QVERIFY(controller.sendScanCode(0x1D, true, true, &errorMessage));
    QVERIFY(controller.sendUnicodeCodeUnit(0xAC00, false, &errorMessage));
    QVERIFY(controller.movePointer(1919, 1079, &errorMessage));
    QVERIFY(controller.movePointerRelative(-5, 8, &errorMessage));
    QVERIFY(controller.setMouseButton(InputMouseButton::Left, true, &errorMessage));
    QVERIFY(controller.setMouseButton(InputMouseButton::Right, false, &errorMessage));
    QVERIFY(controller.setMouseButton(InputMouseButton::Middle, true, &errorMessage));
    QVERIFY(controller.setMouseButton(InputMouseButton::X1, false, &errorMessage));
    QVERIFY(controller.setMouseButton(InputMouseButton::X2, true, &errorMessage));
    QVERIFY(controller.scroll(InputWheelAxis::Vertical, 120, &errorMessage));
    QVERIFY(controller.scroll(InputWheelAxis::Horizontal, -120, &errorMessage));

    QCOMPARE(injectedInputs.size(), std::size_t(11));
    QCOMPARE(injectedInputs[0].type, DWORD(INPUT_KEYBOARD));
    QCOMPARE(injectedInputs[0].ki.wScan, WORD(0x1D));
    QCOMPARE(injectedInputs[0].ki.dwFlags,
             DWORD(KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY));

    QCOMPARE(injectedInputs[1].type, DWORD(INPUT_KEYBOARD));
    QCOMPARE(injectedInputs[1].ki.wScan, WORD(0xAC00));
    QCOMPARE(injectedInputs[1].ki.dwFlags, DWORD(KEYEVENTF_UNICODE));

    QCOMPARE(injectedInputs[2].type, DWORD(INPUT_MOUSE));
    QCOMPARE(injectedInputs[2].mi.dx, LONG(65535));
    QCOMPARE(injectedInputs[2].mi.dy, LONG(65535));
    QCOMPARE(injectedInputs[2].mi.dwFlags,
             DWORD(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK));

    QCOMPARE(injectedInputs[3].mi.dx, LONG(-5));
    QCOMPARE(injectedInputs[3].mi.dy, LONG(8));
    QCOMPARE(injectedInputs[3].mi.dwFlags, DWORD(MOUSEEVENTF_MOVE));

    QCOMPARE(injectedInputs[4].mi.dwFlags, DWORD(MOUSEEVENTF_LEFTDOWN));
    QCOMPARE(injectedInputs[5].mi.dwFlags, DWORD(MOUSEEVENTF_RIGHTUP));
    QCOMPARE(injectedInputs[6].mi.dwFlags, DWORD(MOUSEEVENTF_MIDDLEDOWN));
    QCOMPARE(injectedInputs[7].mi.dwFlags, DWORD(MOUSEEVENTF_XUP));
    QCOMPARE(injectedInputs[7].mi.mouseData, DWORD(XBUTTON1));
    QCOMPARE(injectedInputs[8].mi.dwFlags, DWORD(MOUSEEVENTF_XDOWN));
    QCOMPARE(injectedInputs[8].mi.mouseData, DWORD(XBUTTON2));
    QCOMPARE(injectedInputs[9].mi.dwFlags, DWORD(MOUSEEVENTF_WHEEL));
    QCOMPARE(static_cast<LONG>(injectedInputs[9].mi.mouseData), LONG(120));
    QCOMPARE(injectedInputs[10].mi.dwFlags, DWORD(MOUSEEVENTF_HWHEEL));
    QCOMPARE(static_cast<LONG>(injectedInputs[10].mi.mouseData), LONG(-120));

    std::unique_ptr<InputController> platformController = createInputController();
    QVERIFY(platformController);

    WindowsInputController failingController(
        [](UINT, const INPUT *, int) {
            SetLastError(ERROR_ACCESS_DENIED);
            return UINT(0);
        });
    QVERIFY(!failingController.sendScanCode(0x20, false, false, &errorMessage));
    QVERIFY(errorMessage.contains(QStringLiteral("Windows error 5")));
#else
    QSKIP("Windows input injection is only available on Windows.");
#endif
}

void RdpServerTest::connectsWithTlsAndReconnects()
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

    const TestConnectionResult firstConnection = connectAndReceiveFrames(port);
    QVERIFY2(firstConnection.connected, qPrintable(firstConnection.error));
    QVERIFY(firstConnection.tlsNegotiated);
    QVERIFY2(firstConnection.frameCount >= 1, qPrintable(firstConnection.error));
    QVERIFY(firstConnection.firstFrameHash != 0);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);

    const TestConnectionResult secondConnection = connectAndReceiveFrames(port);
    QVERIFY2(secondConnection.connected, qPrintable(secondConnection.error));
    QVERIFY(secondConnection.tlsNegotiated);
    QVERIFY2(secondConnection.frameCount >= 1, qPrintable(secondConnection.error));
    QVERIFY(secondConnection.firstFrameHash != 0);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 2, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);

    server.stop();
    QVERIFY(!server.isListening());
}

QTEST_GUILESS_MAIN(RdpServerTest)

#include "tst_rdpserver.moc"
