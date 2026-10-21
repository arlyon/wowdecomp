#include <cstdint>
#include "ConsoleDevice.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/Ui/GameUI.h"

// VERIFY: 6674b551 auto
// OFFSET: 0x00769FF0
int32_t CCGxRestart(const char* command, const char* arguments) {
    C2iVector windowSize{};
    const bool windowOwnedSize = g_theGxDevicePtr->DeviceGetWindowLogicalSize(windowSize);
#if !defined(_WIN32)
    OsWindowPlacement placement;
    const bool windowPlacement = OsGuiGetWindowPlacement(g_theGxDevicePtr->DeviceWindow(), placement);
    bool syncWindowMode = false;
    // Preserve live WM state on unrelated Apply/Defaults; an explicitly staged
    // window/fullscreen change still takes precedence over the current state.
    if (s_requestedFormat.m_window == s_lastGoodFormat.m_window &&
        s_requestedFormat.m_maximize == s_lastGoodFormat.m_maximize) {
        OsWindowMode mode;
        if (OsGuiGetWindowMode(g_theGxDevicePtr->DeviceWindow(), mode)) {
            const bool windowed = mode != OsWindowMode::ExclusiveFullscreen;
            const int maximize = mode == OsWindowMode::WindowedFullscreen ? 1 :
                s_requestedFormat.m_maximize == 2 ? 2 : 0;
            syncWindowMode = s_requestedFormat.m_window != windowed || s_requestedFormat.m_maximize != maximize;
            s_requestedFormat.m_window = windowed;
            s_requestedFormat.m_maximize = maximize;
        }
    }
#endif
    const auto applyFormat = [&]() {
        // A window-manager resize must survive Apply and mode-fallback retries.
        // Drawable dimensions can differ on HiDPI and must not size the window.
        if (windowOwnedSize) s_requestedFormat.m_size = windowSize;
#if !defined(_WIN32)
        if (windowPlacement) {
            if (!ConsoleDeviceResolveWindowFormat(s_requestedFormat, placement)) return 0;
        }
#endif
        return g_theGxDevicePtr->DeviceSetFormat(s_requestedFormat);
    };
    OnGxRestartClearWorld();
    ValidateFormatMonitor(s_requestedFormat);

    if (applyFormat()) {
        s_lastGoodFormat = s_requestedFormat;
#if !defined(_WIN32)
        if (syncWindowMode) SetGxCVars(s_requestedFormat);
        else
#endif
        UpdateGxCVars();
    } else {
        ConsoleWrite("unable to set requested display mode", DEFAULT_COLOR);
        s_requestedFormat = s_lastGoodFormat;

        if (!applyFormat()) {
            ConsoleWrite("unable to set last good mode", DEFAULT_COLOR);
            s_requestedFormat = s_desktopFormat;

            if (!applyFormat()) {
                ConsoleWrite("unable to set default format", DEFAULT_COLOR);
                s_requestedFormat = s_fallbackFormat;

                if (!applyFormat()) {
                    goto done;
                }
            }

            s_lastGoodFormat = s_requestedFormat;
        }

        SetGxCVars(s_requestedFormat);
    }

done:
    OsGuiSetGxWindow(g_theGxDevicePtr->DeviceWindow());
    OsGuiSetWindowTitle(g_theGxDevicePtr->DeviceWindow(), s_windowTitle);
    TextureNotifyGxRestart();

    return 1;
}
