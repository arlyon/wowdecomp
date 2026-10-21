#ifndef WOW_SOURCE_UI_LEGACYDISPLAYCOMPATIBILITY_H
#define WOW_SOURCE_UI_LEGACYDISPLAYCOMPATIBILITY_H
#if !defined(_WIN32)
#include <cstdio>
#include "Engine/Source/ConsoleBase/Posix/DisplaySettings.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/lua-5.1/src/lua.h"

// Stock FrameXML may read retired names before native controls are hidden.
// This is a live read-only compatibility view, not a registered/saved setting.
inline bool PushLegacyDisplayValue(lua_State* state, const char* name) {
    if (!ConsoleIsRetiredDisplayCVar(name)) return false;
    if (!strcasecmp(name, "gxRefresh")) lua_pushstring(state, "0");
    else {
        C2iVector size{1024, 768};
        if (g_theGxDevicePtr && g_theGxDevicePtr->m_context) {
            const auto& output = g_theGxDevicePtr->DeviceDefWindow();
            size = {static_cast<int>(output.maxX - output.minX), static_cast<int>(output.maxY - output.minY)};
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%dx%d", size.x, size.y);
        lua_pushstring(state, text);
    }
    return true;
}
#endif
#endif
