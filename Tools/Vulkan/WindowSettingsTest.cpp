#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/DeviceSelection.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include "Engine/Source/Os/SDL/Window.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

CGxFormat s_requestedFormat, s_lastGoodFormat, s_desktopFormat, s_fallbackFormat;
char s_windowTitle[256] = "Window settings fixture";
namespace {
unsigned clears = 0, updates = 0, textureRestarts = 0;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void PumpWindow(CGxDeviceVulkan& gx) {
    for (int iteration = 0; iteration < 20; ++iteration) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) if (event.type == SDL_WINDOWEVENT) OsGuiDispatchNativeWindowEvent(event.window);
        gx.SceneClear(3, CImVector(4,8,12,255));
        gx.ScenePresent();
        SDL_Delay(10);
    }
}
}
// Only host/session side effects are stubbed. The production graphics restart,
// native device, SDL window and swapchain paths run unchanged.
void OnGxRestartClearWorld() { ++clears; }
void ValidateFormatMonitor(CGxFormat&) {}
bool UpdateGxCVars() { ++updates; return true; }
bool SetGxCVars(const CGxFormat&) { return true; }
void TextureNotifyGxRestart() { ++textureRestarts; }
void ConsoleWrite(const char* message, COLOR_T) { throw std::runtime_error(message); }

int main() {
    try {
        Check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        CGxDeviceVulkan gx;
        g_theGxDevicePtr = &gx;
        C2iVector absent{-1,-1};
        Check(!gx.DeviceGetWindowLogicalSize(absent) && absent.x == -1 && absent.y == -1,
            "Uncreated device invented window dimensions");
        CGxFormat format;
        format.m_window = 1; format.m_maximize = 0; format.m_size = {320,240};
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr), format) != 0, "Device creation failed");
        gx.m_cursorVisible = 0;
        auto* window = static_cast<SDL_Window*>(gx.DeviceWindow());
        Check(window != nullptr, "Missing native window");
        SDL_SetWindowSize(window, 576, 360);
        SDL_PumpEvents();
        gx.SceneClear(3, CImVector(4,8,12,255)); gx.ScenePresent();
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        Check(width == 576 && height == 360, "Headless window resize was not applied");
        s_requestedFormat = s_lastGoodFormat = s_desktopFormat = s_fallbackFormat = format;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            // CVar/requested mode intentionally remains stale after a WM resize.
            s_requestedFormat.m_size = {320,240};
            Check(CCGxRestart(nullptr,nullptr) != 0, "Graphics settings restart failed");
            int afterWidth = 0, afterHeight = 0;
            SDL_GetWindowSize(window, &afterWidth, &afterHeight);
            Check(afterWidth == width && afterHeight == height,
                "Applying graphics settings restored stale window resolution");
            Check(s_requestedFormat.m_size.x == width && s_requestedFormat.m_size.y == height,
                "Settings used drawable pixels instead of logical window dimensions");
            gx.SceneClear(3, CImVector(4,8,12,255));
            C2iVector logical{};
            Check(gx.DeviceGetWindowLogicalSize(logical) && logical.x == width && logical.y == height,
                "Window query changed during settings restart");
            int drawableWidth = 0, drawableHeight = 0;
            SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
            const auto& output = gx.DeviceDefWindow();
            Check(int(std::lround(output.maxX-output.minX)) == drawableWidth &&
                int(std::lround(output.maxY-output.minY)) == drawableHeight,
                "UI/output dimensions follow scaled scene rather than drawable window");
            gx.ScenePresent();
        }
        Check(clears == 3 && updates == 3 && textureRestarts == 3, "Restart host calls changed");
        // Explicit backend size requests remain supported; only video-preference
        // restarts preserve the actual window, not every DeviceSetFormat call.
        format.m_size = {400,300};
        Check(gx.DeviceSetFormat(format) != 0, "Explicit backend resize rejected");
        SDL_GetWindowSize(window, &width, &height);
        Check(width == 400 && height == 300, "Explicit backend resize was silently ignored");
        PumpWindow(gx);
        OsWindowPlacement normal;
        Check(OsGuiGetWindowPlacement(window, normal) && normal.width == 400 && normal.height == 300,
            "Normal placement snapshot failed");
        SDL_MaximizeWindow(window); PumpWindow(gx);
        Check(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED, "Fixture compositor did not maximize window");
        s_requestedFormat = format;
        Check(CCGxRestart(nullptr, nullptr), "Maximized Apply failed");
        Check(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED, "Apply unmaximized the window");
        SDL_RestoreWindow(window); PumpWindow(gx);
        SDL_GetWindowSize(window, &width, &height);
        std::cout << "Maximize restore actual=" << width << 'x' << height << " expected=" << normal.width << 'x' << normal.height << '\n';
        Check(width == normal.width && height == normal.height, "Maximize/Apply/restore lost normal geometry");
        s_requestedFormat = format; s_requestedFormat.m_maximize = 1;
        Check(CCGxRestart(nullptr, nullptr), "Windowed fullscreen transition failed");
        PumpWindow(gx);
        Check((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP,
            "Windowed fullscreen intent changed");
        Check(CCGxRestart(nullptr, nullptr), "Fullscreen Apply failed");
        s_requestedFormat.m_maximize = 0;
        Check(CCGxRestart(nullptr, nullptr), "Fullscreen restore failed");
        PumpWindow(gx);
        SDL_GetWindowSize(window, &width, &height);
        OsWindowPlacement restored;
        OsGuiGetWindowPlacement(window, restored);
        std::cout << "Fullscreen restore actual=" << width << 'x' << height << " expected=" << normal.width << 'x' << normal.height
            << " cached=" << restored.width << 'x' << restored.height << " maximized=" << restored.maximized << '\n';
        Check(width == normal.width && height == normal.height, "Fullscreen/Apply/restore lost normal geometry");
        Check(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0, "WM fullscreen setup failed");
        PumpWindow(gx);
        Check(CCGxRestart(nullptr, nullptr), "WM fullscreen Apply failed");
        Check((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP,
            "Unrelated Apply discarded live WM fullscreen state");
        s_requestedFormat.m_maximize = 0;
        Check(CCGxRestart(nullptr, nullptr), "WM fullscreen restore failed");
        PumpWindow(gx);
        SDL_GetWindowSize(window, &width, &height);
        Check(width == normal.width && height == normal.height, "WM fullscreen restore lost normal geometry");
        CGxFormat exclusive = format;
        exclusive.m_window = false;
        Check(ConsoleDeviceResolveWindowFormat(exclusive, normal), "Native monitor mode query failed");
        if (!OsGuiSupportsExclusiveFullscreen()) {
            Check(!gx.DeviceSetFormat(exclusive) && gx.DeviceWindow() == window && gx.m_context,
                "Unsupported exclusive intent succeeded or destroyed the playing device");
            std::cout << "Exclusive fullscreen honestly unavailable on SDL driver " << SDL_GetCurrentVideoDriver() << '\n';
        } else {
            Check(gx.DeviceSetFormat(exclusive), "Exclusive fullscreen request failed");
            Check((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN,
                "Exclusive fullscreen silently became borderless");
            Check(gx.DeviceSetFormat(format), "Exclusive normal restore failed");
            PumpWindow(gx);
            SDL_GetWindowSize(window, &width, &height);
            Check(width == normal.width && height == normal.height, "Exclusive mode overwrote normal geometry");
        }
        int drawableWidth, drawableHeight;
        SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        std::cout << "SDL driver=" << SDL_GetCurrentVideoDriver() << " window=" << width << 'x' << height
            << " drawable=" << drawableWidth << 'x' << drawableHeight << " nativeMode="
            << exclusive.m_size.x << 'x' << exclusive.m_size.y << '@' << exclusive.m_refreshRate << '\n';
        const auto firstIdentity = GxGetDeviceSelection().activeAdapter;
        Check(!firstIdentity.empty(), "Production startup did not report its physical device");
        gx.DeviceDestroy();
        for (const auto& requested : {firstIdentity, std::string("invalid-uuid")}) {
            // A client restart constructs a fresh device; never hot-swap the playing device.
            CGxDeviceVulkan startup;
            g_theGxDevicePtr = &startup;
            GxConfigureDeviceSelection("vulkan", requested.c_str());
            Check(startup.DeviceCreate(static_cast<GxWindowProc>(nullptr), format) != 0, "Explicit/recovery startup failed");
            const auto& selected = GxGetDeviceSelection();
            Check(selected.requestedAdapter == requested, "Startup rewrote saved desired adapter");
            Check(selected.activeAdapter == firstIdentity, "Startup explicit UUID selected a different physical device");
            Check(selected.activeBackend == "vulkan", "Wrong active backend");
            const auto match = std::find_if(selected.adapters.begin(), selected.adapters.end(),
                [&](const auto& d) { return d.id == selected.activeAdapter; });
            Check(match != selected.adapters.end() && match->compatible && match->graphics && match->present && match->swapchain,
                "Actual device lacks graphics/present/swapchain proof");
            if (requested == "invalid-uuid") Check(!selected.warning.empty(), "Invalid requested UUID recovered silently");
            std::cout << "Production Initialize requested=" << requested << " actual=" << selected.activeAdapter
                << " name=" << selected.activeName << " proof=" << match->reason << '\n';
            startup.SceneClear(3, CImVector(4,8,12,255)); startup.ScenePresent();
            startup.DeviceDestroy();
        }
        g_theGxDevicePtr = nullptr; SDL_Quit();
        std::cout << "PASS window-owned resolution: production settings restart preserves logical window size and drawable-sized UI; explicit backend resize remains supported\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
