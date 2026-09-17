#include "clipboardcontroller.h"

#if defined(Q_OS_WIN)
#include "windowsclipboardcontroller_p.h"
#endif

std::unique_ptr<ClipboardController> createClipboardController()
{
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsClipboardController>();
#else
    return nullptr;
#endif
}
