#include "inputcontroller.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
#include "windowsinputcontroller_p.h"
#endif

std::unique_ptr<InputController> createInputController()
{
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsInputController>();
#else
    return nullptr;
#endif
}
