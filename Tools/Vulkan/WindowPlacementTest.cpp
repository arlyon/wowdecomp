#include "Engine/Source/ConsoleBase/Posix/DisplaySettings.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include <SDL.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int Archive(const char* name, const char*, void*) {
    Check(!ConsoleIsRetiredDisplayCVar(name), "Retired CVar persisted");
    return 1;
}
}
int main() {
    try {
        for (const auto* value : {"640x480", "1600x1000", "-1x-2", "999999999999x1", "invalid"}) {
            // Uses production config ingestion and console SET, not a parser replica.
            std::ofstream config("window-placement-test.wtf");
            config << "SET gxResolution \"" << value << "\"\nSET gxRefresh \"999\"\n";
            config.close();
            CVar::Initialize("window-placement-test.wtf");
            Check(CVar::m_needsSave, "Retired config was not marked for migration on next save");
            for (const auto* refresh : {"0", "75", "999", "invalid"}) {
                std::string command = std::string("SET gxRefresh \"") + refresh + "\"";
                ConsoleCommandExecute(command.c_str(), 0);
                Check(!CVar::Lookup("gxRefresh"), "Refresh input survived retirement");
            }
            Check(!CVar::Lookup("gxResolution"), "Config size survived retirement");
            Check(!CVar::Register("GXRESOLUTION", "", 3, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false),
                "Explicit registration resurrected retired setting");
            Check(CVar::IterateForArchive(0, 0, Archive, nullptr), "Archive failed");
            CVar::m_needsSave = false;
            CVar::Destroy();
        }
        std::remove("window-placement-test.wtf");
        for (const auto* invalid : {"", "-1,0,-3,400,0", "0,0,999999,400,0", "0,0,800,600,2", "0,0,800,600,0junk", "0,0,9999999999999999999999,600,0"}) {
            const auto p = OsParseWindowPlacement(invalid);
            Check(p.width == 1024 && p.height == 768 && !p.maximized, "Malformed placement accepted");
        }
        const OsMonitorBounds monitors[] = {{0,0,1920,1080}, {1920,0,1280,720}};
        Check(OsSelectWindowMonitor({2000,20,800,500,false}, monitors, 2) == 1, "Restored monitor not selected");
        Check(OsSelectWindowMonitor({-30000,-30000,800,500,false}, monitors, 2) == 0, "Missing monitor did not fall back to primary");
        auto p = OsParseWindowPlacement("-100,20,800,500,1");
        Check(p.width == 800 && p.maximized, "Saved normal placement lost");
        p = OsBoundWindowPlacement(p, 100, 100, 640, 480);
        Check(p.x == 100 && p.y == 100 && p.width == 640 && p.height == 480, "Monitor fallback not bounded");
        Check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        auto* window = SDL_CreateWindow("Placement fixture", 20, 30, 576, 360, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        Check(window, SDL_GetError());
        OsGuiSetGxWindow(window);
        Check(OsGuiGetWindowPlacement(window, p) && p.width == 576 && p.height == 360, "Normal geometry query failed");
        SDL_DestroyWindow(window);
        window = SDL_CreateWindow("Recreated", 0, 0, 320, 240, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        OsGuiRestoreWindowPlacement(window, p, true);
        int w, h; SDL_GetWindowSize(window, &w, &h);
        Check(w == 576 && h == 360, "Recreation lost normal geometry");
        OsGuiSetGxWindow(nullptr); SDL_DestroyWindow(window); SDL_Quit();
        std::cout << "PASS production CVar config retirement, placement parsing/bounds and SDL normal recreation\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
