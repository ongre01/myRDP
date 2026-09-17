#ifndef CLIPBOARDCONTROLLER_H
#define CLIPBOARDCONTROLLER_H

#include <QString>
#include <QtTypes>

#include <functional>
#include <memory>

class ClipboardController
{
public:
    virtual ~ClipboardController() = default;

    virtual bool changeId(quint64 *id, QString *errorMessage) const = 0;
    virtual bool readText(QString *text,
                          bool *hasText,
                          QString *errorMessage) const = 0;
    virtual bool writeText(const QString &text, QString *errorMessage) = 0;
};

using ClipboardControllerFactory =
    std::function<std::unique_ptr<ClipboardController>()>;

std::unique_ptr<ClipboardController> createClipboardController();

#endif // CLIPBOARDCONTROLLER_H
