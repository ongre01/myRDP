#include "windowsclipboardcontroller_p.h"

#include <QChar>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr int clipboardOpenAttempts = 10;

QString windowsErrorMessage(const QString &operation, DWORD errorCode)
{
    wchar_t *messageBuffer = nullptr;
    const DWORD characterCount = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<wchar_t *>(&messageBuffer),
        0,
        nullptr);
    QString systemMessage;
    if (characterCount > 0 && messageBuffer) {
        systemMessage = QString::fromWCharArray(messageBuffer,
                                                static_cast<qsizetype>(characterCount))
                            .trimmed();
    }
    if (messageBuffer) {
        LocalFree(messageBuffer);
    }

    if (systemMessage.isEmpty()) {
        return QStringLiteral("%1 failed (Windows error %2).")
            .arg(operation)
            .arg(errorCode);
    }
    return QStringLiteral("%1 failed (Windows error %2: %3).")
        .arg(operation)
        .arg(errorCode)
        .arg(systemMessage);
}

bool openClipboardWithRetry(QString *errorMessage)
{
    for (int attempt = 0; attempt < clipboardOpenAttempts; ++attempt) {
        if (OpenClipboard(nullptr)) {
            return true;
        }
        if (attempt + 1 < clipboardOpenAttempts) {
            Sleep(5);
        }
    }

    if (errorMessage) {
        *errorMessage = windowsErrorMessage(QStringLiteral("OpenClipboard"), GetLastError());
    }
    return false;
}

QString toWindowsLineEndings(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('\r')) {
            if (index + 1 < text.size() && text.at(index + 1) == QLatin1Char('\n')) {
                ++index;
            }
            normalized.append(QStringLiteral("\r\n"));
        } else if (character == QLatin1Char('\n')) {
            normalized.append(QStringLiteral("\r\n"));
        } else {
            normalized.append(character);
        }
    }
    return normalized;
}

QString toPortableLineEndings(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('\r')) {
            if (index + 1 < text.size() && text.at(index + 1) == QLatin1Char('\n')) {
                ++index;
            }
            normalized.append(QLatin1Char('\n'));
        } else {
            normalized.append(character);
        }
    }
    return normalized;
}

class ClipboardCloseGuard final
{
public:
    ~ClipboardCloseGuard()
    {
        CloseClipboard();
    }
};
} // namespace

bool WindowsClipboardController::changeId(quint64 *id, QString *errorMessage) const
{
    if (!id) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The clipboard change identifier output is null.");
        }
        return false;
    }

    *id = static_cast<quint64>(GetClipboardSequenceNumber());
    return true;
}

bool WindowsClipboardController::readText(QString *text,
                                          bool *hasText,
                                          QString *errorMessage) const
{
    if (!text || !hasText) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The clipboard text output is null.");
        }
        return false;
    }

    text->clear();
    *hasText = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    if (!*hasText) {
        return true;
    }
    if (!openClipboardWithRetry(errorMessage)) {
        return false;
    }
    ClipboardCloseGuard closeGuard;

    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (!clipboardData) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("GetClipboardData"),
                                                GetLastError());
        }
        return false;
    }

    const SIZE_T byteCount = GlobalSize(clipboardData);
    if (byteCount < sizeof(wchar_t)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The Windows clipboard contains invalid Unicode text.");
        }
        return false;
    }

    const auto *characters = static_cast<const wchar_t *>(GlobalLock(clipboardData));
    if (!characters) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("GlobalLock"), GetLastError());
        }
        return false;
    }

    const SIZE_T capacity = byteCount / sizeof(wchar_t);
    SIZE_T length = 0;
    while (length < capacity && characters[length] != L'\0') {
        ++length;
    }
    if (length == capacity) {
        GlobalUnlock(clipboardData);
        if (errorMessage) {
            *errorMessage = QStringLiteral("The Windows clipboard Unicode text is not terminated.");
        }
        return false;
    }
    if (length > static_cast<SIZE_T>((std::numeric_limits<qsizetype>::max)())) {
        GlobalUnlock(clipboardData);
        if (errorMessage) {
            *errorMessage = QStringLiteral("The Windows clipboard text is too large.");
        }
        return false;
    }

    *text = toPortableLineEndings(
        QString::fromWCharArray(characters, static_cast<qsizetype>(length)));
    GlobalUnlock(clipboardData);
    return true;
}

bool WindowsClipboardController::writeText(const QString &text, QString *errorMessage)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t));

    const QString windowsText = toWindowsLineEndings(text);
    const quint64 characterCount = static_cast<quint64>(windowsText.size()) + 1;
    if (characterCount > (std::numeric_limits<SIZE_T>::max)() / sizeof(wchar_t)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The clipboard text is too large.");
        }
        return false;
    }

    const SIZE_T byteCount = static_cast<SIZE_T>(characterCount * sizeof(wchar_t));
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!memory) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("GlobalAlloc"), GetLastError());
        }
        return false;
    }

    void *destination = GlobalLock(memory);
    if (!destination) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("GlobalLock"), GetLastError());
        }
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, windowsText.utf16(), windowsText.size() * sizeof(char16_t));
    std::memset(static_cast<char *>(destination) + windowsText.size() * sizeof(char16_t),
                0,
                sizeof(wchar_t));
    GlobalUnlock(memory);

    if (!openClipboardWithRetry(errorMessage)) {
        GlobalFree(memory);
        return false;
    }
    ClipboardCloseGuard closeGuard;
    if (!EmptyClipboard()) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("EmptyClipboard"), GetLastError());
        }
        GlobalFree(memory);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        if (errorMessage) {
            *errorMessage = windowsErrorMessage(QStringLiteral("SetClipboardData"), GetLastError());
        }
        GlobalFree(memory);
        return false;
    }

    return true;
}
