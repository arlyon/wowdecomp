#include "Screen.h"
#include <cstdint>
#include <cstdio>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#if !defined(_WIN32) || defined(WOWDECOMP_VULKAN)
#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#endif

// VERIFY: a937de73 auto
// OFFSET: 0x0050F7C0
float Screen::GetAspectRatio() {
#if !defined(_WIN32) || defined(WOWDECOMP_VULKAN)
    if (g_theGxDevicePtr && g_theGxDevicePtr->m_context) {
        // Tiling compositors resize the window independently of gxResolution.
        // Use the presentation window, not a temporarily bound render target.
        const CRect& rect = g_theGxDevicePtr->DeviceDefWindow();
        return CoordinateWindowAspect(rect.maxX - rect.minX, rect.maxY - rect.minY);
    }
#endif
    CVar* widescreen = CVar::Lookup("widescreen");
    CVar* resolution = CVar::Lookup("gxResolution");
    int32_t width = 4;
    int32_t height = 3;
    char separator;

    if (widescreen && widescreen->m_intValue && resolution) {
        sscanf(resolution->m_stringValue.m_str, "%d%c%d", &width, &separator, &height);
    }

    return static_cast<float>(width) / height;
}
