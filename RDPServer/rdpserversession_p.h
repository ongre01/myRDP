#ifndef RDPSERVERSESSION_P_H
#define RDPSERVERSESSION_P_H

#include "clipboardcontroller.h"
#include "desktopcapture.h"
#include "inputcontroller.h"

#include <QByteArray>
#include <QString>
#include <QtTypes>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <freerdp/server/cliprdr.h>
#include <winpr/stream.h>
#include <winpr/wtypes.h>

struct rdp_freerdp_peer;
using freerdp_peer = struct rdp_freerdp_peer;
struct rdp_input;
using rdpInput = struct rdp_input;
struct S_NSC_CONTEXT;
using NSC_CONTEXT = struct S_NSC_CONTEXT;
class RdpClipboardHandler;
class RdpInputHandler;
struct ClipboardTextState;

class RdpServerSession final
{
public:
    using ClosedHandler = std::function<void(quint64, const QString &)>;
    using ErrorHandler = std::function<void(quint64, const QString &)>;

    RdpServerSession(quint64 id,
                     freerdp_peer *peer,
                     ClosedHandler closedHandler,
                     ErrorHandler errorHandler,
                     DesktopCaptureFactory captureFactory = createDesktopCapture,
                     InputControllerFactory inputFactory = createInputController,
                     ClipboardControllerFactory clipboardFactory = createClipboardController);
    ~RdpServerSession();

    RdpServerSession(const RdpServerSession &) = delete;
    RdpServerSession &operator=(const RdpServerSession &) = delete;

    void takePeerOwnership();
    bool start();
    void requestStop();
    void join();

    quint64 id() const;
    QString peerAddress() const;
    bool isRunning() const;

private:
    static BOOL peerPostConnect(freerdp_peer *peer);
    static BOOL peerActivate(freerdp_peer *peer);
    static BOOL inputKeyboardEvent(rdpInput *input, UINT16 flags, UINT8 code);
    static BOOL inputUnicodeKeyboardEvent(rdpInput *input, UINT16 flags, UINT16 code);
    static BOOL inputMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
    static BOOL inputRelativeMouseEvent(rdpInput *input,
                                        UINT16 flags,
                                        INT16 deltaX,
                                        INT16 deltaY);
    static BOOL inputExtendedMouseEvent(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
    static UINT clipboardClientCapabilities(
        CliprdrServerContext *context,
        const CLIPRDR_CAPABILITIES *capabilities);
    static UINT clipboardClientFormatList(
        CliprdrServerContext *context,
        const CLIPRDR_FORMAT_LIST *formatList);
    static UINT clipboardClientFormatListResponse(
        CliprdrServerContext *context,
        const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse);
    static UINT clipboardClientFormatDataRequest(
        CliprdrServerContext *context,
        const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest);
    static UINT clipboardClientFormatDataResponse(
        CliprdrServerContext *context,
        const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse);

    bool handlePostConnect();
    bool initializePeer();
    void cleanupPeer();
    bool initializeClipboardChannel();
    void cleanupClipboardChannel();
    bool pollClipboard();
    UINT sendClipboardFormatList(const ClipboardTextState &state);
    UINT sendClipboardFormatListResponse(bool accepted);
    UINT requestClientClipboardText();
    UINT sendClipboardDataResponse(bool accepted, const QByteArray &encoded = {});
    bool applyDesktopSize(const DesktopSize &size, bool notifyClient);
    bool sendDesktopFrame(bool forceFullFrame);
    void reportError(const QString &message);
    void run();

    quint64 sessionId;
    freerdp_peer *peer;
    QString address;
    ClosedHandler closedHandler;
    ErrorHandler errorHandler;
    DesktopCaptureFactory captureFactory;
    InputControllerFactory inputFactory;
    ClipboardControllerFactory clipboardFactory;
    std::unique_ptr<DesktopCapture> desktopCapture;
    std::unique_ptr<InputController> inputController;
    std::unique_ptr<RdpInputHandler> inputHandler;
    std::unique_ptr<ClipboardController> clipboardController;
    std::unique_ptr<RdpClipboardHandler> clipboardHandler;
    bool ownsPeer = false;
    bool started = false;
    bool peerInitialized = false;
    std::atomic_bool stopRequested = false;
    std::atomic_bool running = false;
    std::atomic_bool activated = false;
    std::atomic_bool fullFrameRequested = false;
    std::atomic_bool errorReported = false;
    std::atomic_bool clipboardReady = false;
    HANDLE virtualChannelManager = nullptr;
    CliprdrServerContext *clipboardContext = nullptr;
    bool clipboardStarted = false;
    UINT32 requestedClientClipboardFormat = 0;
    std::mutex clipboardSendMutex;
    NSC_CONTEXT *nscContext = nullptr;
    wStream *frameStream = nullptr;
    quint32 desktopWidth = 0;
    quint32 desktopHeight = 0;
    std::thread worker;
};

#endif // RDPSERVERSESSION_P_H
