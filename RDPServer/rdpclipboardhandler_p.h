#ifndef RDPCLIPBOARDHANDLER_P_H
#define RDPCLIPBOARDHANDLER_P_H

#include "clipboardcontroller.h"

#include <QByteArray>
#include <QString>

#include <mutex>

enum class ClipboardPollStatus
{
    Unchanged,
    Changed,
    Error
};

struct ClipboardTextState
{
    bool hasText = false;
    QString text;
};

class RdpClipboardHandler final
{
public:
    explicit RdpClipboardHandler(ClipboardController &controller);

    bool initialize(QString *errorMessage = nullptr);
    ClipboardPollStatus pollLocalClipboard(ClipboardTextState *state,
                                           QString *errorMessage = nullptr);
    bool encodedLocalText(QByteArray *encoded, QString *errorMessage = nullptr);
    bool applyRemoteText(const QByteArray &encoded, QString *errorMessage = nullptr);
    bool clearRemoteText(QString *errorMessage = nullptr);

    static QByteArray encodeUtf16Le(const QString &text);
    static QString decodeUtf16Le(const QByteArray &encoded, bool *ok = nullptr);

private:
    bool readStableState(quint64 *changeId,
                         ClipboardTextState *state,
                         QString *errorMessage);
    bool recordCurrentChange(QString *errorMessage);

    ClipboardController &controller;
    bool hasObservedChange = false;
    quint64 observedChangeId = 0;
    std::mutex mutex;
};

#endif // RDPCLIPBOARDHANDLER_P_H
