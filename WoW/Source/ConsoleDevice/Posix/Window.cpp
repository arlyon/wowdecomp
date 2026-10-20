#if !defined(_WIN32)
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Os/Window.h"

bool ConsoleDeviceResolveWindowFormat(CGxFormat& format, const OsWindowPlacement& placement) {
    format.m_pos = {placement.x, placement.y};
    if (format.m_window) {
        format.m_size = {placement.width, placement.height};
        format.m_refreshRate = 0;
        return true;
    }
    // Exclusive mode owns a monitor extent, never the normal restore rectangle.
    // Desktop mode, not the current mode potentially changed by another client.
    return OsGuiGetWindowDesktopMode(OsGuiGetWindow(0), placement, format.m_size, format.m_refreshRate);
}
#endif
