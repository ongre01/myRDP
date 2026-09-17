#include "clipboarduibridge.h"

#include <QClipboard>
#include <QMetaObject>
#include <QScopedValueRollback>
#include <QThread>

ClipboardUiBridge::ClipboardUiBridge(QClipboard *clipboard, QObject *parent)
    : QObject(parent)
    , clipboard(clipboard)
{
    Q_ASSERT(clipboard);
}

void ClipboardUiBridge::applyRemoteText(const QString &text)
{
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
            Q_ASSERT(QThread::currentThread() == thread());
            if (!clipboard || clipboard->text() == text) {
                return;
            }

            pendingRemoteEcho = text;
            QScopedValueRollback<bool> applyingClipboardText(applyingRemoteText, true);
            clipboard->setText(text);
        },
        Qt::QueuedConnection);
}

bool ClipboardUiBridge::shouldForwardLocalChange(const QString &text)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (applyingRemoteText) {
        return false;
    }

    if (pendingRemoteEcho && *pendingRemoteEcho == text) {
        pendingRemoteEcho.reset();
        return false;
    }

    pendingRemoteEcho.reset();
    return true;
}
