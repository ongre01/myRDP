#include "autostartmanager.h"
#include "clipboardcontroller.h"
#include "desktopcapture.h"
#include "desktopframebuffer_p.h"
#include "inputcontroller.h"
#include "rdpclipboardhandler_p.h"
#include "rdpclient.h"
#include "rdpinputhandler_p.h"
#include "rdpserver.h"
#include "serverconfiguration.h"
#include "serverlogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#if defined(Q_OS_WIN)
#include "windowsclipboardcontroller_p.h"
#include "windowsinputcontroller_p.h"
#endif

#include <freerdp/codec/color.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

class RecordingClipboardController final : public ClipboardController
{
public:
    bool changeId(quint64 *id, QString *errorMessage) const override
    {
        if (failChangeId || !id) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Clipboard change id failed.");
            }
            return false;
        }
        *id = sequence;
        return true;
    }

    bool readText(QString *text,
                  bool *available,
                  QString *errorMessage) const override
    {
        if (failRead || !text || !available) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Clipboard read failed.");
            }
            return false;
        }
        *text = value;
        *available = hasText;
        return true;
    }

    bool writeText(const QString &text, QString *errorMessage) override
    {
        if (failWrite) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Clipboard write failed.");
            }
            return false;
        }
        value = text;
        hasText = true;
        ++sequence;
        ++writeCount;
        return true;
    }

    void setLocalText(const QString &text)
    {
        value = text;
        hasText = true;
        ++sequence;
    }

    void removeLocalText()
    {
        value.clear();
        hasText = false;
        ++sequence;
    }

    quint64 sequence = 1;
    QString value;
    bool hasText = false;
    bool failChangeId = false;
    bool failRead = false;
    bool failWrite = false;
    int writeCount = 0;
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

struct IntegrationState
{
    void recordInput(const RecordedInput &input)
    {
        std::lock_guard<std::mutex> lock(mutex);
        inputs.push_back(input);
    }

    bool hasExpectedInput() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto hasRecord = [this](const std::function<bool(const RecordedInput &)> &matches) {
            return std::any_of(inputs.cbegin(), inputs.cend(), matches);
        };
        return hasRecord([](const RecordedInput &input) {
                   return input.type == RecordedInputType::ScanCode && input.first == 0x1E
                          && !input.releasedOrPressed;
               })
               && hasRecord([](const RecordedInput &input) {
                   return input.type == RecordedInputType::PointerMove && input.first == 100
                          && input.second == 120;
               })
               && hasRecord([](const RecordedInput &input) {
                   return input.type == RecordedInputType::MouseButton
                          && input.button == InputMouseButton::Left
                          && input.releasedOrPressed;
               })
               && hasRecord([](const RecordedInput &input) {
                   return input.type == RecordedInputType::MouseButton
                          && input.button == InputMouseButton::Left
                          && !input.releasedOrPressed;
               })
               && hasRecord([](const RecordedInput &input) {
                   return input.type == RecordedInputType::Wheel
                          && input.axis == InputWheelAxis::Vertical && input.first == -120;
               });
    }

    bool clipboardEquals(const QString &expected, int minimumWrites) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return clipboardWriteCount >= minimumWrites && clipboardHasText
               && clipboardText == expected;
    }

    void setLocalClipboardText(const QString &text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        clipboardText = text;
        clipboardHasText = true;
        ++clipboardSequence;
    }

    mutable std::mutex mutex;
    std::vector<RecordedInput> inputs;
    QString clipboardText;
    quint64 clipboardSequence = 1;
    bool clipboardHasText = false;
    int clipboardWriteCount = 0;
};

class IntegrationDesktopCapture final : public DesktopCapture
{
public:
    DesktopSize desktopSize(QString *) override
    {
        return size;
    }

    DesktopCaptureResult capture(bool forceFullFrame) override
    {
        DesktopFrame frame;
        frame.desktopSize = size;
        const bool fullFrame = forceFullFrame || frameNumber == 0;
        if (fullFrame) {
            frame.width = size.width;
            frame.height = size.height;
        } else {
            frame.x = 32;
            frame.y = 24;
            frame.width = 16;
            frame.height = 16;
        }
        frame.stride = frame.width * desktopCaptureBytesPerPixel;
        frame.pixels.resize(static_cast<std::size_t>(frame.stride) * frame.height);

        const std::uint8_t blue = fullFrame ? 0x31 : 0xC7;
        const std::uint8_t green = static_cast<std::uint8_t>(0x40 + (frameNumber % 0x40));
        for (std::size_t offset = 0; offset < frame.pixels.size(); offset += 4) {
            frame.pixels[offset] = blue;
            frame.pixels[offset + 1] = green;
            frame.pixels[offset + 2] = 0xA5;
            frame.pixels[offset + 3] = 0xFF;
        }
        ++frameNumber;
        return {DesktopCaptureStatus::FrameReady, std::move(frame), {}};
    }

private:
    const DesktopSize size{640, 480};
    std::uint64_t frameNumber = 0;
};

class IntegrationInputController final : public InputController
{
public:
    explicit IntegrationInputController(std::shared_ptr<IntegrationState> state)
        : state(std::move(state))
    {
    }

    bool sendScanCode(std::uint16_t scanCode,
                      bool released,
                      bool extended,
                      QString *) override
    {
        state->recordInput(
            {RecordedInputType::ScanCode, scanCode, 0, released, extended});
        return true;
    }

    bool sendUnicodeCodeUnit(std::uint16_t codeUnit, bool released, QString *) override
    {
        state->recordInput({RecordedInputType::Unicode, codeUnit, 0, released});
        return true;
    }

    bool movePointer(std::uint16_t x, std::uint16_t y, QString *) override
    {
        state->recordInput({RecordedInputType::PointerMove, x, y});
        return true;
    }

    bool movePointerRelative(std::int16_t deltaX, std::int16_t deltaY, QString *) override
    {
        state->recordInput({RecordedInputType::RelativePointerMove, deltaX, deltaY});
        return true;
    }

    bool setMouseButton(InputMouseButton button, bool pressed, QString *) override
    {
        RecordedInput input;
        input.type = RecordedInputType::MouseButton;
        input.releasedOrPressed = pressed;
        input.button = button;
        state->recordInput(input);
        return true;
    }

    bool scroll(InputWheelAxis axis, std::int16_t delta, QString *) override
    {
        RecordedInput input;
        input.type = RecordedInputType::Wheel;
        input.first = delta;
        input.axis = axis;
        state->recordInput(input);
        return true;
    }

private:
    std::shared_ptr<IntegrationState> state;
};

class IntegrationClipboardController final : public ClipboardController
{
public:
    explicit IntegrationClipboardController(std::shared_ptr<IntegrationState> state)
        : state(std::move(state))
    {
    }

    bool changeId(quint64 *id, QString *) const override
    {
        if (!id) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        *id = state->clipboardSequence;
        return true;
    }

    bool readText(QString *text, bool *hasText, QString *) const override
    {
        if (!text || !hasText) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        *text = state->clipboardText;
        *hasText = state->clipboardHasText;
        return true;
    }

    bool writeText(const QString &text, QString *) override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->clipboardText = text;
        state->clipboardHasText = true;
        ++state->clipboardSequence;
        ++state->clipboardWriteCount;
        return true;
    }

private:
    std::shared_ptr<IntegrationState> state;
};

struct ClientObservations
{
    void addDesktopUpdate(const DesktopUpdate &update)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (desktopUpdates.size() < 32) {
            desktopUpdates.push_back(update);
        }
    }

    void addClipboardText(const QString &text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        clipboardTexts.push_back(text);
    }

    bool receivedDesktopFrameAndRefresh() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool receivedFrame = false;
        bool receivedRefresh = false;
        for (const DesktopUpdate &update : desktopUpdates) {
            const bool hasContent = std::any_of(update.pixels.cbegin(),
                                                update.pixels.cend(),
                                                [](char value) { return value != 0; });
            receivedFrame = receivedFrame
                            || (update.desktopSize == QSize(640, 480)
                                && update.dirtyRect == QRect(0, 0, 640, 480) && hasContent);
            receivedRefresh = receivedRefresh
                              || (update.desktopSize == QSize(640, 480)
                                  && update.dirtyRect == QRect(32, 24, 16, 16) && hasContent);
        }
        return receivedFrame && receivedRefresh;
    }

    bool receivedClipboardText(const QString &expected) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return std::find(clipboardTexts.cbegin(), clipboardTexts.cend(), expected)
               != clipboardTexts.cend();
    }

    void clearDesktopUpdates()
    {
        std::lock_guard<std::mutex> lock(mutex);
        desktopUpdates.clear();
    }

    mutable std::mutex mutex;
    std::vector<DesktopUpdate> desktopUpdates;
    std::vector<QString> clipboardTexts;
};

bool processClientUntil(RdpClient &client,
                        const std::function<bool()> &condition,
                        int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        if (!client.processEvents()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(10);
    }
    return condition();
}
} // namespace

class RdpServerTest : public QObject
{
    Q_OBJECT

private slots:
    void buildsAutomaticStartupCommand();
    void loadsAndValidatesServerConfiguration();
    void filtersAndWritesLogMessages();
    void rejectsInvalidPort();
    void acceptsAndClosesSession();
    void enforcesMaximumClientCount();
    void tracksDesktopFrameChanges();
    void capturesWindowsDesktop();
    void routesRdpInputEvents();
    void convertsWindowsInputEvents();
    void synchronizesClipboardText();
    void createsClipboardController();
    void connectsWithTlsAndReconnects();
    void integratesOwnClientAndServer();
};

void RdpServerTest::buildsAutomaticStartupCommand()
{
    const QString applicationPath = QStringLiteral("C:/Program Files/QtRdp/RDPServer.exe");
    QCOMPARE(AutoStartManager::startupCommand(applicationPath),
             QStringLiteral("\"%1\" --background")
                 .arg(QDir::toNativeSeparators(applicationPath)));
    QVERIFY(AutoStartManager::startupCommand(QString()).isEmpty());

    const AutoStartManager manager(applicationPath);
#if defined(Q_OS_WIN)
    QVERIFY(manager.isSupported());
#else
    QVERIFY(!manager.isSupported());
#endif
}

void RdpServerTest::loadsAndValidatesServerConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString configurationPath = directory.filePath(QStringLiteral("RDPServer.ini"));

    QFile configurationFile(configurationPath);
    QVERIFY(configurationFile.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(configurationFile.write(
                "[Server]\n"
                "ListenAddress=127.0.0.1\n"
                "RdpPort=3391\n"
                "Authentication=Disabled\n"
                "Certificate=\n"
                "PrivateKey=\n"
                "Capture=Desktop\n"
                "MaximumClientCount=3\n"
                "LogLevel=Warning\n"
                "LogFile=logs/server.log\n")
            > 0);
    configurationFile.close();

    RdpServerConfiguration configuration;
    QString errorMessage;
    QVERIFY2(RdpServerSettings::load(configurationPath, &configuration, &errorMessage),
             qPrintable(errorMessage));
    QCOMPARE(configuration.bindAddress, QStringLiteral("127.0.0.1"));
    QCOMPARE(configuration.port, quint32(3391));
    QVERIFY(configuration.authentication == RdpServerAuthentication::Disabled);
    QVERIFY(configuration.captureMode == RdpServerCaptureMode::Desktop);
    QCOMPARE(configuration.maximumClientCount, quint32(3));
    QVERIFY(configuration.logLevel == RdpServerLogLevel::Warning);
    QCOMPARE(configuration.logFilePath,
             directory.filePath(QStringLiteral("logs/server.log")));

    QVERIFY(configurationFile.open(QIODevice::WriteOnly
                                   | QIODevice::Truncate
                                   | QIODevice::Text));
    QVERIFY(configurationFile.write(
                "[Server]\n"
                "ListenAddress=not-an-address\n"
                "RdpPort=70000\n"
                "Authentication=maybe\n"
                "Capture=Unknown\n"
                "MaximumClientCount=0\n"
                "LogLevel=Verbose\n")
            > 0);
    configurationFile.close();

    QVERIFY(!RdpServerSettings::load(configurationPath, &configuration, &errorMessage));
    QVERIFY(!errorMessage.isEmpty());
}

void RdpServerTest::filtersAndWritesLogMessages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logPath = directory.filePath(QStringLiteral("server.log"));

    {
        RdpServerLogger logger;
        QString errorMessage;
        QVERIFY2(logger.configure(RdpServerLogLevel::Warning, logPath, &errorMessage),
                 qPrintable(errorMessage));
        QVERIFY(logger.write(RdpServerLogLevel::Debug,
                             QStringLiteral("Session state"),
                             QStringLiteral("filtered debug event"),
                             &errorMessage));
        QVERIFY(logger.write(RdpServerLogLevel::Warning,
                             QStringLiteral("Network error"),
                             QStringLiteral("warning event"),
                             &errorMessage));
        QVERIFY(logger.write(RdpServerLogLevel::Error,
                             QStringLiteral("RDP protocol error"),
                             QStringLiteral("protocol event"),
                             &errorMessage));
    }

    QFile logFile(logPath);
    QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contents = QString::fromUtf8(logFile.readAll());
    QVERIFY(!contents.contains(QStringLiteral("filtered debug event")));
    QVERIFY(contents.contains(QStringLiteral("[WARNING] [Network error] warning event")));
    QVERIFY(contents.contains(QStringLiteral("[ERROR] [RDP protocol error] protocol event")));
}

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

void RdpServerTest::enforcesMaximumClientCount()
{
    QTcpServer portProbe;
    QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.serverPort();
    portProbe.close();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RdpServer server;
    QVERIFY2(server.isInitialized(), qPrintable(server.lastError()));
    QSignalSpy connectedSpy(&server, &RdpServer::clientConnected);

    RdpServerConfiguration configuration;
    configuration.bindAddress = QStringLiteral("127.0.0.1");
    configuration.port = port;
    configuration.maximumClientCount = 1;
    configuration.logLevel = RdpServerLogLevel::Debug;
    configuration.logFilePath = directory.filePath(QStringLiteral("server.log"));
    QVERIFY2(server.start(configuration), qPrintable(server.lastError()));

    QTcpSocket firstClient;
    firstClient.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(firstClient.waitForConnected(2000));
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(1), 2000);

    QTcpSocket secondClient;
    secondClient.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(secondClient.waitForConnected(2000));
    QTRY_VERIFY_WITH_TIMEOUT(secondClient.state() == QAbstractSocket::UnconnectedState, 2000);
    QCOMPARE(connectedSpy.count(), 1);
    QCOMPARE(server.sessionCount(), qsizetype(1));

    firstClient.disconnectFromHost();
    if (firstClient.state() != QAbstractSocket::UnconnectedState) {
        QVERIFY(firstClient.waitForDisconnected(2000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);
    server.stop();

    QFile logFile(configuration.logFilePath);
    QVERIFY(logFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contents = QString::fromUtf8(logFile.readAll());
    QVERIFY(contents.contains(QStringLiteral("maximum client count")));
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

void RdpServerTest::synchronizesClipboardText()
{
    RecordingClipboardController controller;
    controller.setLocalText(QStringLiteral("initial"));
    RdpClipboardHandler handler(controller);
    QString errorMessage;
    QVERIFY(handler.initialize(&errorMessage));

    ClipboardTextState state;
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Unchanged);

    const QString localText = QString::fromUtf8("서버 clipboard\n두 번째 줄");
    controller.setLocalText(localText);
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Changed);
    QVERIFY(state.hasText);
    QCOMPARE(state.text, localText);

    QByteArray encoded;
    QVERIFY(handler.encodedLocalText(&encoded, &errorMessage));
    QVERIFY(encoded.endsWith(QByteArray::fromHex("0000")));
    QVERIFY(encoded.contains(QByteArray::fromHex("0d000a00")));
    bool decodedSuccessfully = false;
    QCOMPARE(RdpClipboardHandler::decodeUtf16Le(encoded, &decodedSuccessfully), localText);
    QVERIFY(decodedSuccessfully);

    const QString remoteText = QString::fromUtf8("client → server ✓\nline 2");
    QVERIFY(handler.applyRemoteText(RdpClipboardHandler::encodeUtf16Le(remoteText),
                                    &errorMessage));
    QCOMPARE(controller.value, remoteText);
    QCOMPARE(controller.writeCount, 1);
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Unchanged);

    QVERIFY(!handler.applyRemoteText(QByteArray::fromHex("410000"), &errorMessage));
    QVERIFY(!errorMessage.isEmpty());

    controller.removeLocalText();
    errorMessage.clear();
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Changed);
    QVERIFY(!state.hasText);

    QVERIFY(handler.clearRemoteText(&errorMessage));
    QCOMPARE(controller.value, QString());
    QCOMPARE(controller.writeCount, 2);
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Unchanged);

    controller.setLocalText(QStringLiteral("unreadable"));
    controller.failRead = true;
    QCOMPARE(handler.pollLocalClipboard(&state, &errorMessage),
             ClipboardPollStatus::Error);
    QVERIFY(!errorMessage.isEmpty());
}

void RdpServerTest::createsClipboardController()
{
#if defined(Q_OS_WIN)
    std::unique_ptr<ClipboardController> controller = createClipboardController();
    QVERIFY(controller);
    QVERIFY(dynamic_cast<WindowsClipboardController *>(controller.get()));
#else
    QVERIFY(!createClipboardController());
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

void RdpServerTest::integratesOwnClientAndServer()
{
    QTcpServer portProbe;
    QVERIFY(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.serverPort();
    portProbe.close();

    const auto integrationState = std::make_shared<IntegrationState>();
    RdpServerDependencies dependencies;
    dependencies.desktopCaptureFactory = []() {
        return std::make_unique<IntegrationDesktopCapture>();
    };
    dependencies.inputControllerFactory = [integrationState]() {
        return std::make_unique<IntegrationInputController>(integrationState);
    };
    dependencies.clipboardControllerFactory = [integrationState]() {
        return std::make_unique<IntegrationClipboardController>(integrationState);
    };

    RdpServer server(std::move(dependencies));
    QVERIFY2(server.isInitialized(), qPrintable(server.lastError()));
    QSignalSpy connectedSpy(&server, &RdpServer::clientConnected);
    QSignalSpy disconnectedSpy(&server, &RdpServer::clientDisconnected);

    RdpServerConfiguration configuration;
    configuration.bindAddress = QStringLiteral("127.0.0.1");
    configuration.port = port;
    QVERIFY2(server.start(configuration), qPrintable(server.lastError()));

    ConnectionInfo connectionInfo;
    connectionInfo.serverAddress = QStringLiteral("127.0.0.1");
    connectionInfo.port = port;
    connectionInfo.username = QStringLiteral("integration-user");
    connectionInfo.password = QStringLiteral("integration-password");

    bool rejectionPrompted = false;
    CertificateInfo rejectedCertificate;
    connectionInfo.certificateVerifier = [&](const CertificateInfo &certificate) {
        rejectionPrompted = true;
        rejectedCertificate = certificate;
        return CertificateDecision::Reject;
    };
    {
        RdpClient rejectingClient;
        QVERIFY2(rejectingClient.isInitialized(), qPrintable(rejectingClient.lastError()));
        QVERIFY(!rejectingClient.connectToServer(connectionInfo));
        QVERIFY(rejectionPrompted);
        QVERIFY(!rejectedCertificate.fingerprint.isEmpty());
        QVERIFY(rejectingClient.lastError().contains(QStringLiteral("rejected"),
                                                     Qt::CaseInsensitive));
    }
    QTRY_VERIFY_WITH_TIMEOUT(connectedSpy.count() >= 1, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);
    connectedSpy.clear();
    disconnectedSpy.clear();

    ClientObservations observations;
    RdpClient client;
    QVERIFY2(client.isInitialized(), qPrintable(client.lastError()));
    client.setDesktopUpdateHandler(
        [&](const DesktopUpdate &update) { observations.addDesktopUpdate(update); });
    client.setClipboardTextHandler(
        [&](const QString &text) { observations.addClipboardText(text); });

    const QString clientClipboardText = QStringLiteral("Client → Server\n한글 clipboard");
    QVERIFY(client.sendClipboardText(clientClipboardText));

    int certificateApprovalCount = 0;
    CertificateInfo trustedCertificate;
    connectionInfo.certificateVerifier = [&](const CertificateInfo &certificate) {
        ++certificateApprovalCount;
        trustedCertificate = certificate;
        return CertificateDecision::TrustOnce;
    };

    QVERIFY2(client.connectToServer(connectionInfo), qPrintable(client.lastError()));
    QVERIFY(client.isConnected());
    QCOMPARE(certificateApprovalCount, 1);
    QCOMPARE(trustedCertificate.host, QStringLiteral("127.0.0.1"));
    QCOMPARE(trustedCertificate.port, static_cast<int>(port));
    QVERIFY(!trustedCertificate.fingerprint.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);

    QVERIFY2(processClientUntil(client, [&]() {
                 return observations.receivedDesktopFrameAndRefresh();
             }),
             qPrintable(client.lastError() + QStringLiteral(" / ") + server.lastError()));

    RdpKeyboardInput keyboardInput;
    keyboardInput.kind = RdpKeyboardInput::Kind::ScanCode;
    keyboardInput.scanCode = 0x1E;
    keyboardInput.pressed = true;
    QVERIFY2(client.sendKeyboardInput(keyboardInput), qPrintable(client.lastError()));

    RdpPointerInput pointerInput;
    pointerInput.kind = RdpPointerInput::Kind::Move;
    pointerInput.position = QPoint(100, 120);
    QVERIFY2(client.sendPointerInput(pointerInput), qPrintable(client.lastError()));
    pointerInput.kind = RdpPointerInput::Kind::LeftButton;
    pointerInput.pressed = true;
    QVERIFY2(client.sendPointerInput(pointerInput), qPrintable(client.lastError()));
    pointerInput.pressed = false;
    QVERIFY2(client.sendPointerInput(pointerInput), qPrintable(client.lastError()));
    pointerInput.kind = RdpPointerInput::Kind::VerticalWheel;
    pointerInput.wheelDelta = -120;
    QVERIFY2(client.sendPointerInput(pointerInput), qPrintable(client.lastError()));

    QVERIFY2(processClientUntil(client, [&]() { return integrationState->hasExpectedInput(); }),
             qPrintable(client.lastError()));
    QVERIFY2(processClientUntil(client, [&]() {
                 return integrationState->clipboardEquals(clientClipboardText, 1);
             }),
             qPrintable(client.lastError()));

    const QString serverClipboardText = QStringLiteral("Server → Client\n양방향 clipboard");
    integrationState->setLocalClipboardText(serverClipboardText);
    QVERIFY2(processClientUntil(client, [&]() {
                 return observations.receivedClipboardText(serverClipboardText);
             }),
             qPrintable(client.lastError()));

    client.disconnect();
    QVERIFY(!client.isConnected());
    QVERIFY2(client.lastError().isEmpty(), qPrintable(client.lastError()));
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);

    const QString reconnectClipboardText = QStringLiteral("reconnected client clipboard");
    QVERIFY(client.sendClipboardText(reconnectClipboardText));
    observations.clearDesktopUpdates();
    QVERIFY2(client.connectToServer(connectionInfo), qPrintable(client.lastError()));
    QVERIFY(client.isConnected());
    QVERIFY(certificateApprovalCount >= 1);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 2, 2000);
    QVERIFY2(processClientUntil(client, [&]() {
                 return observations.receivedDesktopFrameAndRefresh()
                        && integrationState->clipboardEquals(reconnectClipboardText, 2);
             }),
             qPrintable(client.lastError()));

    client.disconnect();
    QVERIFY(!client.isConnected());
    QVERIFY2(client.lastError().isEmpty(), qPrintable(client.lastError()));
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 2, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(server.sessionCount(), qsizetype(0), 2000);

    server.stop();
    QVERIFY(!server.isListening());
}

QTEST_GUILESS_MAIN(RdpServerTest)

#include "tst_rdpserver.moc"
