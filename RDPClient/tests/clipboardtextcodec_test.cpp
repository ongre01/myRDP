#include "../clipboardtextcodec.h"

#include <QtTest>

class ClipboardTextCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void encodesUnicodeTextAndWindowsLineEndings();
    void decodesUnicodeTextAndQtLineEndings();
    void handlesEmptyClipboard();
    void rejectsOddByteCount();
    void stopsAtNullTerminator();
};

void ClipboardTextCodecTest::encodesUnicodeTextAndWindowsLineEndings()
{
    const QString text = QString::fromUtf8("A\n한글 😀");
    const QByteArray encoded = ClipboardTextCodec::encodeUtf16Le(text);

    QCOMPARE(encoded.left(6), QByteArray::fromHex("41000d000a00"));
    QVERIFY(encoded.endsWith(QByteArray::fromHex("0000")));

    bool ok = false;
    QCOMPARE(ClipboardTextCodec::decodeUtf16Le(encoded, &ok), text);
    QVERIFY(ok);
}

void ClipboardTextCodecTest::decodesUnicodeTextAndQtLineEndings()
{
    const QByteArray encoded = QByteArray::fromHex("41000d000a0042000d0043000000");
    bool ok = false;

    QCOMPARE(ClipboardTextCodec::decodeUtf16Le(encoded, &ok), QStringLiteral("A\nB\nC"));
    QVERIFY(ok);
}

void ClipboardTextCodecTest::handlesEmptyClipboard()
{
    QCOMPARE(ClipboardTextCodec::encodeUtf16Le(QString()), QByteArray::fromHex("0000"));

    bool ok = false;
    QCOMPARE(ClipboardTextCodec::decodeUtf16Le(QByteArray(), &ok), QString());
    QVERIFY(ok);
}

void ClipboardTextCodecTest::rejectsOddByteCount()
{
    bool ok = true;
    QCOMPARE(ClipboardTextCodec::decodeUtf16Le(QByteArray::fromHex("410000"), &ok), QString());
    QVERIFY(!ok);
}

void ClipboardTextCodecTest::stopsAtNullTerminator()
{
    const QByteArray encoded = QByteArray::fromHex("4100000042000000");
    bool ok = false;

    QCOMPARE(ClipboardTextCodec::decodeUtf16Le(encoded, &ok), QStringLiteral("A"));
    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(ClipboardTextCodecTest)

#include "clipboardtextcodec_test.moc"
