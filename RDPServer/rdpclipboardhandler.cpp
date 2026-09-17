#include "rdpclipboardhandler_p.h"

#include <QChar>

#include <limits>

namespace {
constexpr int stableReadAttempts = 3;

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
} // namespace

RdpClipboardHandler::RdpClipboardHandler(ClipboardController &controller)
    : controller(controller)
{
}

bool RdpClipboardHandler::initialize(QString *errorMessage)
{
    std::lock_guard<std::mutex> lock(mutex);
    return recordCurrentChange(errorMessage);
}

ClipboardPollStatus RdpClipboardHandler::pollLocalClipboard(ClipboardTextState *state,
                                                            QString *errorMessage)
{
    if (!state) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The clipboard state output is null.");
        }
        return ClipboardPollStatus::Error;
    }

    std::lock_guard<std::mutex> lock(mutex);
    quint64 currentChangeId = 0;
    if (!controller.changeId(&currentChangeId, errorMessage)) {
        return ClipboardPollStatus::Error;
    }
    if (hasObservedChange && currentChangeId == observedChangeId) {
        return ClipboardPollStatus::Unchanged;
    }

    if (!readStableState(&currentChangeId, state, errorMessage)) {
        return ClipboardPollStatus::Error;
    }
    observedChangeId = currentChangeId;
    hasObservedChange = true;
    return ClipboardPollStatus::Changed;
}

bool RdpClipboardHandler::encodedLocalText(QByteArray *encoded, QString *errorMessage)
{
    if (!encoded) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The encoded clipboard output is null.");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    quint64 currentChangeId = 0;
    ClipboardTextState state;
    if (!readStableState(&currentChangeId, &state, errorMessage)) {
        return false;
    }
    if (!state.hasText) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The local clipboard no longer contains text.");
        }
        return false;
    }

    *encoded = encodeUtf16Le(state.text);
    if (encoded->isNull()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The local clipboard text is too large to encode.");
        }
        return false;
    }
    observedChangeId = currentChangeId;
    hasObservedChange = true;
    return true;
}

bool RdpClipboardHandler::applyRemoteText(const QByteArray &encoded, QString *errorMessage)
{
    bool decodedSuccessfully = false;
    const QString text = decodeUtf16Le(encoded, &decodedSuccessfully);
    if (!decodedSuccessfully) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The RDP client returned invalid UTF-16 clipboard text.");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!controller.writeText(text, errorMessage)) {
        return false;
    }
    return recordCurrentChange(errorMessage);
}

bool RdpClipboardHandler::clearRemoteText(QString *errorMessage)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!controller.writeText(QString(), errorMessage)) {
        return false;
    }
    return recordCurrentChange(errorMessage);
}

QByteArray RdpClipboardHandler::encodeUtf16Le(const QString &text)
{
    const QString normalized = toWindowsLineEndings(text);
    if (normalized.size() > ((std::numeric_limits<qsizetype>::max)() / 2) - 1) {
        return QByteArray();
    }

    QByteArray encoded((normalized.size() + 1) * 2, Qt::Uninitialized);
    auto *destination = reinterpret_cast<uchar *>(encoded.data());
    for (qsizetype index = 0; index < normalized.size(); ++index) {
        const ushort codeUnit = normalized.at(index).unicode();
        destination[index * 2] = static_cast<uchar>(codeUnit & 0xFFU);
        destination[index * 2 + 1] = static_cast<uchar>((codeUnit >> 8U) & 0xFFU);
    }
    destination[normalized.size() * 2] = 0;
    destination[normalized.size() * 2 + 1] = 0;
    return encoded;
}

QString RdpClipboardHandler::decodeUtf16Le(const QByteArray &encoded, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if ((encoded.size() % 2) != 0) {
        return QString();
    }

    const auto *source = reinterpret_cast<const uchar *>(encoded.constData());
    QString decoded;
    decoded.reserve(encoded.size() / 2);
    for (qsizetype index = 0; index < encoded.size(); index += 2) {
        const ushort codeUnit = static_cast<ushort>(source[index])
                                | static_cast<ushort>(source[index + 1] << 8U);
        if (codeUnit == 0) {
            break;
        }
        decoded.append(QChar(codeUnit));
    }

    if (ok) {
        *ok = true;
    }
    return toPortableLineEndings(decoded);
}

bool RdpClipboardHandler::readStableState(quint64 *changeId,
                                          ClipboardTextState *state,
                                          QString *errorMessage)
{
    for (int attempt = 0; attempt < stableReadAttempts; ++attempt) {
        quint64 before = 0;
        quint64 after = 0;
        if (!controller.changeId(&before, errorMessage)
            || !controller.readText(&state->text, &state->hasText, errorMessage)
            || !controller.changeId(&after, errorMessage)) {
            return false;
        }
        if (before == after) {
            *changeId = after;
            return true;
        }
    }

    if (errorMessage) {
        *errorMessage = QStringLiteral("The local clipboard changed repeatedly while it was read.");
    }
    return false;
}

bool RdpClipboardHandler::recordCurrentChange(QString *errorMessage)
{
    quint64 currentChangeId = 0;
    if (!controller.changeId(&currentChangeId, errorMessage)) {
        return false;
    }
    observedChangeId = currentChangeId;
    hasObservedChange = true;
    return true;
}
