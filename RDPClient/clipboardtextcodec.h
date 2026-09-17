#ifndef CLIPBOARDTEXTCODEC_H
#define CLIPBOARDTEXTCODEC_H

#include <QByteArray>
#include <QString>

namespace ClipboardTextCodec {

QByteArray encodeUtf16Le(const QString &text);
QString decodeUtf16Le(const QByteArray &data, bool *ok = nullptr);

} // namespace ClipboardTextCodec

#endif // CLIPBOARDTEXTCODEC_H
