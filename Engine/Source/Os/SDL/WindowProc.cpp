#include "Engine/Source/Os/Input.h"

intptr_t OsWindowProc(void*, uint32_t, uintptr_t, intptr_t) {
    // Non-applicable legacy Win32 default-procedure hook. Linux window events
    // are handled by OsGuiProcessMessage and its owner-thread SDL dispatcher;
    // these numeric Win32 message parameters are not SDL events.
    return 0;
}
