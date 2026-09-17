#include "../clipboarduibridge.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QThread>
#include <QtTest>

class ClipboardUiBridgeTest : public QObject
{
    Q_OBJECT

private slots:
    void appliesWorkerThreadTextOnGuiThreadWithoutEcho();
};

void ClipboardUiBridgeTest::appliesWorkerThreadTextOnGuiThreadWithoutEcho()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    const QString originalText = clipboard->text();

    ClipboardUiBridge bridge(clipboard);
    int forwardedChanges = 0;
    const QMetaObject::Connection clipboardConnection =
        connect(clipboard, &QClipboard::dataChanged, &bridge, [&]() {
            if (bridge.shouldForwardLocalChange(clipboard->text())) {
                ++forwardedChanges;
            }
        });

    QThread worker;
    QObject workerContext;
    workerContext.moveToThread(&worker);
    worker.start();

    const QString remoteText = QString::fromUtf8("Remote 한글 😀");
    const bool invokeSucceeded = QMetaObject::invokeMethod(
        &workerContext,
        [&bridge, remoteText]() { bridge.applyRemoteText(remoteText); },
        Qt::BlockingQueuedConnection);

    QElapsedTimer timeout;
    timeout.start();
    while (clipboard->text() != remoteText && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }

    const QString appliedText = clipboard->text();
    const int forwardedRemoteChanges = forwardedChanges;

    QThread *guiThread = QCoreApplication::instance()->thread();
    const bool moveSucceeded = QMetaObject::invokeMethod(
        &workerContext,
        [&workerContext, guiThread]() { workerContext.moveToThread(guiThread); },
        Qt::BlockingQueuedConnection);

    worker.quit();
    const bool workerStopped = worker.wait(5000);

    disconnect(clipboardConnection);
    clipboard->setText(originalText);

    QVERIFY(invokeSucceeded);
    QCOMPARE(appliedText, remoteText);
    QCOMPARE(forwardedRemoteChanges, 0);
    QVERIFY(moveSucceeded);
    QVERIFY(workerStopped);
}

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    ClipboardUiBridgeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "clipboarduibridge_test.moc"
