#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <SDL.h>
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/SDL/Window.h"

namespace {
SDL_Window* s_window;
OsWindowPlacement s_normal;
bool s_resizeLock;
OsNativeWindowEventProc s_windowProc;
void* s_windowContext;
bool PlacementDisplay(const OsWindowPlacement& placement, int& display) {
    const int count = SDL_GetNumVideoDisplays();
    if (count <= 0) return false;
    std::vector<OsMonitorBounds> monitors;
    for (int index = 0; index < count; ++index) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(index, &bounds) != 0) return false;
        monitors.push_back({bounds.x, bounds.y, bounds.w, bounds.h});
    }
    display = static_cast<int>(OsSelectWindowMonitor(placement, monitors.data(), monitors.size()));
    return true;
}
}

void* OsGuiGetWindow(int32_t type) {
    switch (type) {
        case 0: return s_window;
        case 1:
        case 2: return SDL_GetKeyboardFocus();
        default: return nullptr;
    }
}

void OsGuiSetGxWindow(void* window) {
    if (s_window != window) {
        s_windowProc = nullptr;
        s_windowContext = nullptr;
    }
    s_window = static_cast<SDL_Window*>(window);
    if (s_window) SDL_SetWindowResizable(s_window, s_resizeLock ? SDL_FALSE : SDL_TRUE);
}

void OsGuiSetNativeWindowEventProc(OsNativeWindowEventProc proc, void* context) {
    s_windowProc = proc;
    s_windowContext = proc ? context : nullptr;
}

void OsGuiDispatchNativeWindowEvent(const SDL_WindowEvent& event) {
    if (s_window && event.windowID == SDL_GetWindowID(s_window)) {
        OsWindowPlacement placement;
        OsGuiGetWindowPlacement(s_window, placement);
    }
    if (s_windowProc) s_windowProc(s_windowContext, event);
}

int32_t OsGetDefaultWindowRect(tagRECT* rect) {
    if (!rect || !s_window) {
        errno = EINVAL;
        return 0;
    }
    int width, height;
    SDL_GetWindowSize(s_window, &width, &height);
    if (width <= 0 || height <= 0) return 0;
    *rect = {0, 0, width, height};
    return 1;
}

void OsInputSetWindowResizeLock(int32_t resizeLock) {
    s_resizeLock = resizeLock != 0;
    if (s_window) SDL_SetWindowResizable(s_window, s_resizeLock ? SDL_FALSE : SDL_TRUE);
}

void OsGuiSetWindowTitle(void* window, const char* title) {
    SDL_SetWindowTitle(static_cast<SDL_Window*>(window), title);
}

int32_t OsGuiIsModifierKeyDown(int32_t key) {
    const SDL_Keymod modifiers = SDL_GetModState();
    switch (key) {
        case 0: return (modifiers & KMOD_CTRL) != 0;
        case 1: return (modifiers & KMOD_SHIFT) != 0;
        case 2: return (modifiers & KMOD_ALT) != 0;
        default: return 0;
    }
}

int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title) {
    const SDL_MessageBoxButtonData ok[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"},
    };
    const SDL_MessageBoxButtonData okCancel[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    const SDL_MessageBoxButtonData yesNo[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "No"},
    };
    const SDL_MessageBoxButtonData yesNoCancel[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Yes"}, {0, 1, "No"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = static_cast<SDL_Window*>(parentWindow);
    box.title = title ? title : "";
    box.message = message ? message : "";
    switch (style) {
        case 1: box.buttons = okCancel; box.numbuttons = 2; break;
        case 2: box.buttons = yesNo; box.numbuttons = 2; break;
        case 3: box.buttons = yesNoCancel; box.numbuttons = 3; break;
        default: box.buttons = ok; box.numbuttons = 1; break;
    }
    int button = 2;
    if (SDL_ShowMessageBox(&box, &button) != 0) {
        std::fprintf(stderr, "OsGuiMessageBox: %s\n", SDL_GetError());
        return 2;
    }
    return button < 0 ? 2 : button;
}

void OsGuiBeep() {
    std::fputc('\a', stderr);
}

bool OsGuiSetSavedWindowPlacement(const OsWindowPlacement& placement) {
    int display;
    if (!PlacementDisplay(placement, display)) return false;
    SDL_Rect bounds{};
    if (SDL_GetDisplayUsableBounds(display, &bounds) != 0 || bounds.w <= 0 || bounds.h <= 0) return false;
    s_normal = OsBoundWindowPlacement(placement, bounds.x, bounds.y, bounds.w, bounds.h);
    return true;
}

OsWindowPlacement OsGuiGetSavedWindowPlacement() { return s_normal; }

bool OsGuiGetWindowPlacement(void* handle, OsWindowPlacement& placement) {
    auto* window = static_cast<SDL_Window*>(handle);
    if (!window) return false;
    const auto flags = SDL_GetWindowFlags(window);
    if (!(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
        s_normal.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        if (!s_normal.maximized) {
            SDL_GetWindowPosition(window, &s_normal.x, &s_normal.y);
            SDL_GetWindowSize(window, &s_normal.width, &s_normal.height);
        }
    }
    placement = s_normal;
    return placement.width > 0 && placement.height > 0;
}

void OsGuiRestoreWindowPlacement(void* handle, const OsWindowPlacement& placement, bool normalMode) {
    auto* window = static_cast<SDL_Window*>(handle);
    if (!window) return;
    s_normal = placement;
    SDL_SetWindowPosition(window, placement.x, placement.y);
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    if (width != placement.width || height != placement.height)
        SDL_SetWindowSize(window, placement.width, placement.height);
    if (normalMode && placement.maximized) SDL_MaximizeWindow(window);
}

bool OsGuiGetWindowDesktopMode(void* handle, const OsWindowPlacement& placement, C2iVector& size, uint32_t& refresh) {
    int display = handle ? SDL_GetWindowDisplayIndex(static_cast<SDL_Window*>(handle)) : -1;
    if (display < 0 && !PlacementDisplay(placement, display)) return false;
    SDL_DisplayMode mode{};
    if (SDL_GetDesktopDisplayMode(display, &mode) != 0 || mode.w <= 0 || mode.h <= 0) return false;
    size = {mode.w, mode.h};
    refresh = mode.refresh_rate;
    return true;
}

bool OsGuiSupportsExclusiveFullscreen() {
    const auto* driver = SDL_GetCurrentVideoDriver();
    return driver && std::strcmp(driver, "wayland") && std::strcmp(driver, "dummy");
}

bool OsGuiSetWindowMode(void* handle, bool windowed, int maximize, const C2iVector& size, uint32_t refresh) {
    auto* window = static_cast<SDL_Window*>(handle);
    // Wayland exposes compositor fullscreen, not exclusive monitor mode ownership.
    if (!windowed && !OsGuiSupportsExclusiveFullscreen()) {
        SDL_SetError("Exclusive fullscreen is unsupported by this SDL adapter; select windowed fullscreen explicitly");
        return false;
    }
    const uint32_t requested = !windowed ? SDL_WINDOW_FULLSCREEN : maximize == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    const auto previous = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (!windowed) {
        SDL_DisplayMode mode{};
        mode.w = size.x; mode.h = size.y; mode.refresh_rate = refresh;
        SDL_DisplayMode current{};
        if (SDL_GetWindowDisplayMode(window, &current) != 0) return false;
        if ((current.w != mode.w || current.h != mode.h || (refresh && current.refresh_rate != mode.refresh_rate))
            && SDL_SetWindowDisplayMode(window, &mode) != 0) return false;
    }
    if (previous != requested && SDL_SetWindowFullscreen(window, requested) != 0) return false;
    if (!windowed) {
        SDL_DisplayMode actual{};
        if (SDL_GetWindowDisplayMode(window, &actual) != 0) return false;
        if (actual.w != size.x || actual.h != size.y || (refresh && actual.refresh_rate != static_cast<int>(refresh)) ||
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != SDL_WINDOW_FULLSCREEN) {
            SDL_SetError("SDL did not fulfill the requested exclusive display mode");
            return false;
        }
    }
    return true;
}

bool OsGuiGetWindowMode(void* handle, OsWindowMode& mode) {
    if (!handle) return false;
    const auto flags = SDL_GetWindowFlags(static_cast<SDL_Window*>(handle)) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    mode = flags == SDL_WINDOW_FULLSCREEN_DESKTOP ? OsWindowMode::WindowedFullscreen :
        flags == SDL_WINDOW_FULLSCREEN ? OsWindowMode::ExclusiveFullscreen : OsWindowMode::Normal;
    return true;
}
