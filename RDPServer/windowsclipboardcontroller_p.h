#ifndef WINDOWSCLIPBOARDCONTROLLER_P_H
#define WINDOWSCLIPBOARDCONTROLLER_P_H

#include "clipboardcontroller.h"

class WindowsClipboardController final : public ClipboardController
{
public:
    bool changeId(quint64 *id, QString *errorMessage) const override;
    bool readText(QString *text,
                  bool *hasText,
                  QString *errorMessage) const override;
    bool writeText(const QString &text, QString *errorMessage) override;
};

#endif // WINDOWSCLIPBOARDCONTROLLER_P_H
