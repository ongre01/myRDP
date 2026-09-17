#include "clipboardtextcodec.h"

#include <QtGlobal>

namespace {

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
            normalized.append(QLatin1Char('\r'));
            normalized.append(QLatin1Char('\n'));
        } else if (character == QLatin1Char('\n')) {
            normalized.append(QLatin1Char('\r'));
            normalized.append(QLatin1Char('\n'));
        } else {
            normalized.append(character);
        }
    }

    return normalized;
}

QString toQtLineEndings(const QString &text)
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

namespace ClipboardTextCodec {

QByteArray encodeUtf16Le(const QString &text)
{
    const QString normalized = toWindowsLineEndings(text);
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

QString decodeUtf16Le(const QByteArray &data, bool *ok)
{
    if (ok) {
        *ok = false;
    }

    if ((data.size() % 2) != 0) {
        return QString();
    }

    const auto *source = reinterpret_cast<const uchar *>(data.constData());
    QString decoded;
    decoded.reserve(data.size() / 2);

    for (qsizetype index = 0; index < data.size(); index += 2) {
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
    return toQtLineEndings(decoded);
}

} // namespace ClipboardTextCodec
