#include <SDL.h>
#include "Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/SDL/Window.h"

namespace {
void SetClientRect(CGxDeviceD3d& device, CGxFormat* format = nullptr) {
    int width, height;
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(device.m_hwnd), &width, &height);
    CRect rect;
    rect.minX = rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    device.DeviceSetDefWindow(rect);
    if (format && format->m_window) {
        SDL_GetWindowSize(static_cast<SDL_Window*>(device.m_hwnd), &width, &height);
        format->m_size = {width, height};
    }
}

void WindowEvent(void* context, const SDL_WindowEvent& event) {
    auto& device = *static_cast<CGxDeviceD3d*>(context);
    switch (event.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            device.DeviceWM(GxWM_SetFocus, 0, 0);
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            device.DeviceWM(GxWM_KillFocus, 0, 0);
            device.ISetWindowFocus(0);
            break;
        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_MINIMIZED:
            device.DeviceWM(GxWM_Size, 0, 1);
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED: {
            int width, height;
            SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(device.m_hwnd), &width, &height);
            if (width <= 0 || height <= 0) break;
            const CRect& current = device.DeviceDefWindow();
            if (device.m_windowVisible && current.maxX == width && current.maxY == height) break;
            CRect rect;
            rect.minX = rect.minY = 0.0f;
            rect.maxX = static_cast<float>(width);
            rect.maxY = static_cast<float>(height);
            device.DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
            break;
        }
        default: break;
    }
}

void DetachWindow(CGxDeviceD3d& device) {
    if (OsGuiGetWindow(0) == device.m_hwnd) OsGuiSetGxWindow(nullptr);
    if (device.m_hwnd && device.m_ownhwnd) SDL_DestroyWindow(static_cast<SDL_Window*>(device.m_hwnd));
    device.m_hwnd = nullptr;
}
}

int32_t CGxDeviceD3d::ILoadD3dLib(HINSTANCE& library, LPDIRECT3D9& d3d) {
    library = nullptr;
    d3d = nullptr;
    // Native D3D9 is linked, not loaded as a Windows DLL. Its WSI handle must
    // agree with the SDL2 window pointers used by this platform implementation.
    if (SDL_setenv("DXVK_WSI_DRIVER", "SDL2", 1) != 0) return 0;
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    return d3d != nullptr;
}

void CGxDeviceD3d::IUnloadD3dLib(HINSTANCE& library, LPDIRECT3D9& d3d) {
    if (d3d) d3d->Release();
    d3d = nullptr;
    library = nullptr;
}

int32_t CGxDeviceD3d::ICreateWindow(CGxFormat& format) {
    if (format.m_size.x < 0 || format.m_size.y < 0) return 0;
    uint32_t flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (format.m_window && format.m_maximize == 2) flags |= SDL_WINDOW_BORDERLESS;
    else flags |= SDL_WINDOW_RESIZABLE;
    SDL_Window* window = SDL_CreateWindow("World of Warcraft", format.m_pos.x, format.m_pos.y,
        format.m_size.x ? format.m_size.x : 1024, format.m_size.y ? format.m_size.y : 768, flags);
    if (!window) {
        CGxDevice::Log("SDL_CreateWindow: %s", SDL_GetError());
        return 0;
    }
    auto placement = OsGuiGetSavedWindowPlacement();
    if (format.m_window && !format.m_maximize) {
        placement.x = format.m_pos.x; placement.y = format.m_pos.y;
        placement.width = format.m_size.x ? format.m_size.x : 1024;
        placement.height = format.m_size.y ? format.m_size.y : 768;
    }
    OsGuiRestoreWindowPlacement(window, placement, format.m_window && !format.m_maximize);
    const auto fullscreen = !format.m_window ? SDL_WINDOW_FULLSCREEN :
        format.m_maximize == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    if (fullscreen && !OsGuiSetWindowMode(window, format.m_window, format.m_maximize, format.m_size, format.m_refreshRate)) {
        CGxDevice::Log("SDL display mode request failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        return 0;
    }
    this->m_hwnd = window;
    this->m_hwndClass = 0;
    SDL_SetWindowMinimumSize(window, 320, 240);
    CGxDevice::s_clientAdjust = {0, 0};
    OsGuiSetGxWindow(window);
    SetClientRect(*this, &format);
    if (SDL_GetWindowGammaRamp(window, this->m_systemGammaRamp.m_red,
            this->m_systemGammaRamp.m_green, this->m_systemGammaRamp.m_blue) == 0) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }
    return 1;
}

void CGxDeviceD3d::ISetWindowFocus(int32_t focus) {
    this->m_windowFocus = focus != 0;
    if (focus) this->m_hwCursorDirty = 1;
    if (this->m_hwnd) {
        SDL_SetWindowMouseGrab(static_cast<SDL_Window*>(this->m_hwnd),
            focus && !this->m_format.m_window ? SDL_TRUE : SDL_FALSE);
    }
}

void CGxDeviceD3d::CursorSetVisible(int32_t visible) {
    CGxDevice::CursorSetVisible(visible);
    if (this->m_d3dDevice && this->m_context) {
        this->m_d3dDevice->ShowCursor(this->m_hwCursor && this->m_cursorVisible);
        SDL_ShowCursor(this->m_hwCursor && this->m_cursorVisible ? SDL_ENABLE : SDL_DISABLE);
    }
}

void CGxDeviceD3d::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = this->DeviceCurWindow();
    if (this->IDevIsWindowed() && this->m_hwnd) {
        int x, y;
        // Wayland does not expose global window positions. Use the placement
        // coordinates SDL can provide, without querying other applications.
        SDL_GetWindowPosition(static_cast<SDL_Window*>(this->m_hwnd), &x, &y);
        rect.minX += x;
        rect.maxX += x;
        rect.minY += y;
        rect.maxY += y;
    }
}

int32_t CGxDeviceD3d::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceD3d::DeviceSetFormat():");
    this->Log(format);
    if (!format.m_window && !OsGuiSupportsExclusiveFullscreen()) {
        CGxDevice::Log("Exclusive fullscreen unsupported by the current SDL video driver");
        return 0;
    }
    if (OsGuiGetWindow(0) == this->m_hwnd) OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    OsWindowPlacement placement;
    const bool restorePlacement = OsGuiGetWindowPlacement(this->m_hwnd, placement);
    const auto previousFlags = this->m_hwnd ? SDL_GetWindowFlags(static_cast<SDL_Window*>(this->m_hwnd)) : 0;
    if (restorePlacement && format.m_window && !format.m_maximize &&
        !(previousFlags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
        if (format.m_size.x > 0 && format.m_size.y > 0) {
            placement.width = format.m_size.x; placement.height = format.m_size.y;
        }
    }
    if (restorePlacement && !OsGuiSetSavedWindowPlacement(placement)) return 0;
    this->IDestroyD3d();
    this->m_context = 0;
    this->m_windowVisible = 0;
    if (this->m_ownhwnd) DetachWindow(*this);
    this->m_format = format;
    CGxFormat actual = format;
    if (restorePlacement) {
        placement = OsGuiGetSavedWindowPlacement();
        actual.m_pos = {placement.x, placement.y};
        if (actual.m_window) actual.m_size = {placement.width, placement.height};
    }
    bool windowReady = false;
    if (this->m_ownhwnd) {
        windowReady = this->ICreateWindow(actual) != 0;
    } else if (this->m_hwnd) {
        SetClientRect(*this, &actual);
        OsGuiSetGxWindow(this->m_hwnd);
        windowReady = true;
    }
    if (windowReady && this->ICreateD3dDevice(actual) && CGxDevice::DeviceSetFormat(actual)) {
        OsGuiSetNativeWindowEventProc(WindowEvent, this);
        if (actual.m_maximize != 2) SDL_ShowWindow(static_cast<SDL_Window*>(this->m_hwnd));
        this->m_windowVisible = actual.m_maximize != 2;
        int width, height, drawableWidth, drawableHeight;
        auto* window = static_cast<SDL_Window*>(this->m_hwnd);
        SDL_GetWindowSize(window, &width, &height);
        SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);
        SDL_DisplayMode mode{};
        const int display = SDL_GetWindowDisplayIndex(window);
        const bool modeKnown = display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0;
        CGxDevice::Log("SDL D3D window=%dx%d drawable=%dx%d display=%d modeKnown=%d mode=%dx%d@%d flags=%u (SDL-reported, not hardware exclusivity proof)",
            width, height, drawableWidth, drawableHeight, display, modeKnown, mode.w, mode.h,
            mode.refresh_rate, SDL_GetWindowFlags(window));
        this->ISetWindowFocus((SDL_GetWindowFlags(static_cast<SDL_Window*>(this->m_hwnd)) & SDL_WINDOW_INPUT_FOCUS) != 0);
        return 1;
    }
    CGxDevice::Log("CGxDeviceD3d::DeviceSetFormat(): unable to set native format");
    this->IDestroyD3d();
    this->m_context = 0;
    DetachWindow(*this);
    return 0;
}

void CGxDeviceD3d::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    if (OsGuiGetWindow(0) == this->m_hwnd) OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    // Release swapchains/surfaces before destroying their SDL window.
    this->IDestroyD3d();
    this->m_context = 0;
    this->m_windowVisible = 0;
    this->m_windowFocus = 0;
    DetachWindow(*this);
    this->m_hwndClass = 0;
    this->IUnloadD3dLib(this->m_d3dLib, this->m_d3d);
}

int32_t CGxDeviceD3d::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;
    this->m_hwndClass = 0;
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        CGxDevice::Log("SDL video must be initialized before creating the graphics device");
        return 0;
    }
    if (this->ICreateD3d() && CGxDevice::DeviceCreate(windowProc, format)) return 1;
    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceD3d::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    this->m_hwndClass = 0;
    this->m_hwnd = window;
    if (!window) return 0;
    CGxDevice::DeviceCreate(window, format);
    SetClientRect(*this);
    OsGuiSetGxWindow(window);
    if (this->ICreateD3d() && this->ICreateD3dDevice(format)) {
        OsGuiSetNativeWindowEventProc(WindowEvent, this);
        this->m_windowVisible = !(SDL_GetWindowFlags(static_cast<SDL_Window*>(window)) & SDL_WINDOW_HIDDEN);
        this->ISetWindowFocus((SDL_GetWindowFlags(static_cast<SDL_Window*>(window)) & SDL_WINDOW_INPUT_FOCUS) != 0);
        return 1;
    }
    this->DeviceDestroy();
    return 0;
}

bool CGxDeviceD3d::DeviceGetWindowLogicalSize(C2iVector& size) const {
    if (!m_hwnd) return false;
    int width, height;
    SDL_GetWindowSize(static_cast<SDL_Window*>(m_hwnd), &width, &height);
    if (width <= 0 || height <= 0) return false;
    size = {width, height};
    return true;
}
