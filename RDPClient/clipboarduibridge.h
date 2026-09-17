#ifndef CLIPBOARDUIBRIDGE_H
#define CLIPBOARDUIBRIDGE_H

#include <QObject>
#include <QString>

#include <optional>

class QClipboard;

class ClipboardUiBridge : public QObject
{
public:
    explicit ClipboardUiBridge(QClipboard *clipboard, QObject *parent = nullptr);

    void applyRemoteText(const QString &text);
    bool shouldForwardLocalChange(const QString &text);

private:
    QClipboard *clipboard = nullptr;
    bool applyingRemoteText = false;
    std::optional<QString> pendingRemoteEcho;
};

#endif // CLIPBOARDUIBRIDGE_H
