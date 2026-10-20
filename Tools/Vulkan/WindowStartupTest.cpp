#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"
#include "WoW/Source/ConsoleDevice/ConsoleDetect.h"
#include "WoW/Source/Client.h"
#include "WoW/Source/Hardware.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Base/Types.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/DeviceSelection.h"
#include <cstdlib>
#include "Engine/Source/Os/Window.h"
#include <SDL.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

int32_t s_locale = 4;
WowClientDB<Startup_StringsRec> g_startup_StringsDB;
namespace {
struct Captured {};
CGxFormat captured;
CGxFormat defaults(true, {1600, 1200}, CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Ds24X,
    75, 1, true, false, true, true, 0);
bool hardwareChanged;
VideoHardwareRec video;
bool checkSelection = false;
unsigned hardwareProbes = 0;
GxDeviceSelection expectedSelection;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void CheckSelection() {
    if (!checkSelection) return;
    const auto& actual = GxGetDeviceSelection();
    Check(!actual.backends.empty(), "Hardware probe preceded selector bootstrap");
    Check(actual.requestedBackend == expectedSelection.requestedBackend &&
        actual.requestedAdapter == expectedSelection.requestedAdapter, "Saved selectors lost before probe/create");
    Check(actual.effectiveBackend == expectedSelection.effectiveBackend &&
        actual.effectiveAdapter == expectedSelection.effectiveAdapter, "Probe/create policy differs from resolved selection");
    Check(actual.warning == expectedSelection.warning, "Recovery reason lost between probe and creation");
    EGxApi expectedApi = GxApi_D3d9;
#if defined(WOWDECOMP_VULKAN)
    if (actual.effectiveBackend == "vulkan") expectedApi = GxApi_Vulkan;
#endif
    Check(GxDefaultApi() == expectedApi, "Adapter probe API differs from selected creation API");
}
}
// Stop at the production startup/backend boundary: no device/game session runs.
extern "C" CGxDevice* CaptureCreate(EGxApi, GxWindowProc, const CGxFormat& format)
    asm("__wrap__Z14CreateGxDevice6EGxApiPFlPvjmlERK9CGxFormat");
extern "C" CGxDevice* CaptureCreate(EGxApi api, GxWindowProc, const CGxFormat& format) {
    CheckSelection();
    if (checkSelection) Check(api == GxDefaultApi(), "CreateDevice differs from probe backend");
    captured = format;
    throw Captured{};
}
void ConsoleDetectDetectHardware(Hardware& hardware, bool& changed) {
    CheckSelection();
    ++hardwareProbes;
    video.m_multisample = 1; video.m_d3DOverrides = video.m_oglOverrides = "";
    hardware.m_videoHw = &video; changed = hardwareChanged;
}
void ConsoleDetectSetDefaultsFormat(DefaultSettings& settings, const Hardware&) { settings.m_format = &defaults; }
void ConsoleDetectSetDefaults(DefaultSettings& settings, const Hardware&) { settings.m_format = &defaults; }
int32_t CmdLineGetBool(CMDOPT option) {
    // All retired resolution switches are set; production startup must ignore them.
    return option == static_cast<CMDOPT>(CMD_RES_800x600) || option == static_cast<CMDOPT>(CMD_RES_1024x768)
        || option == static_cast<CMDOPT>(CMD_RES_1280x960) || option == static_cast<CMDOPT>(CMD_RES_1280x1024)
        || option == static_cast<CMDOPT>(CMD_RES_1600x1200);
}
const char* CmdLineGetString(CMDOPT) { return ""; }
void OnGxRestartClearWorld() {}
void TextureNotifyGxRestart() {}

int main() {
    try {
        unsetenv("WOWDECOMP_GX_BACKEND"); unsetenv("WOWDECOMP_GX_ADAPTER");
#if defined(WOWDECOMP_VULKAN)
        Check(GxDefaultApi() == GxApi_Vulkan, "Unconfigured native default changed");
#else
        Check(GxDefaultApi() == GxApi_D3d9, "Unconfigured DXVK default changed");
#endif
        CGxFormat native;
        native.m_window = false;
        Check(!ConsoleDeviceResolveWindowFormat(native, {}), "Missing native monitor mode reported success");
        Check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        OsWindowPlacement restore{20,30,800,500,false};
        Check(ConsoleDeviceResolveWindowFormat(native, restore), "Native desktop mode was unavailable");
        SDL_DisplayMode desktop;
        Check(SDL_GetDesktopDisplayMode(0, &desktop) == 0 && native.m_size.x == desktop.w &&
            native.m_size.y == desktop.h && native.m_refreshRate == static_cast<uint32_t>(desktop.refresh_rate),
            "Exclusive intent used normal placement rather than desktop mode");
        Check(restore.width == 800 && restore.height == 500, "Native mode overwrote normal restore size");
        for (int detect : {0, 1}) for (bool changed : {false, true}) {
            hardwareChanged = changed;
            for (const auto* stale : {"640x480", "1600x1000", "malformed", "-1x-2", "999999999999x1"})
            for (const auto* refresh : {"0", "75", "999", "malformed"}) {
                std::ofstream config("window-startup-test.wtf");
                config << "SET hwDetect \"" << detect << "\"\nSET gxWindow \"1\"\nSET gxMaximize \"0\"\n"
                    << "SET gxResolution \"" << stale << "\"\nSET gxRefresh \"" << refresh << "\"\n"
                    << "SET windowPlacement \"20,30,800,500,0\"\n";
                config.close();
                CVar::Initialize("window-startup-test.wtf");
                try { ConsoleDeviceInitialize("Startup fixture", false, false); Check(false, "Startup did not reach backend"); }
                catch (const Captured&) {}
                Check(captured.m_size.x == 800 && captured.m_size.y == 500, "Old mode/hardware defaults controlled startup geometry");
                Check(captured.m_refreshRate == 0, "Old refresh controlled startup");
                Check(captured.m_pos.x == 20 && captured.m_pos.y == 30, "Saved normal placement lost");
                Check(!CVar::Lookup("gxResolution") && !CVar::Lookup("gxRefresh"), "Retired settings re-registered");
                CVar::m_needsSave = false; CVar::Destroy();
            }
        }
        // Execute the real registry/config/bootstrap repeatedly, as fresh client
        // restarts would, with stale in-process selection deliberately left behind.
        const std::string first = "01000000000000000000000000000000:3";
        const std::string missing = "ffffffffffffffffffffffffffffffff:3";
        std::vector<GxBackendDescriptor> backends;
#if defined(WOWDECOMP_VULKAN)
        backends.push_back({"vulkan", "Native Vulkan"});
#endif
#if defined(WOWDECOMP_DXVK)
        backends.push_back({"dxvk", "D3D9 / DXVK"});
#endif
        struct Case { const char* backend; std::string adapter; const char* envBackend; std::string envAdapter; };
        for (const auto& scenario : std::vector<Case>{
                {"auto", "auto", "", ""}, {"vulkan", first, "", ""}, {"dxvk", first, "", ""},
                {"invalid", "invalid", "", ""}, {"auto", missing, "", ""},
                {"dxvk", missing, "vulkan", first}, {"vulkan", first, "dxvk", missing},
                {"dxvk", first, "invalid", "invalid"}, {"auto", "auto", "auto", "auto"}}) {
            setenv("WOWDECOMP_GX_BACKEND", scenario.envBackend, 1);
            setenv("WOWDECOMP_GX_ADAPTER", scenario.envAdapter.c_str(), 1);
            expectedSelection = GxResolveSelection(backends, scenario.backend, scenario.adapter,
                scenario.envBackend, scenario.envAdapter);
            // Prove startup refreshes, rather than accidentally reusing the last run.
            GxConfigureDeviceSelection("auto", "auto");
            std::ofstream config("window-startup-test.wtf");
            config << "SET hwDetect \"0\"\nSET gxWindow \"1\"\nSET gxMaximize \"0\"\n"
                << "SET windowPlacement \"20,30,800,500,0\"\n"
                << "SET gxBackend \"" << scenario.backend << "\"\n"
                << "SET gxAdapter \"" << scenario.adapter << "\"\n";
            config.close();
            CVar::Initialize("window-startup-test.wtf");
            checkSelection = true;
            const unsigned before = hardwareProbes;
            try { ConsoleDeviceInitialize("Selector startup fixture", false, false); Check(false, "Startup did not reach backend"); }
            catch (const Captured&) {}
            Check(hardwareProbes == before + 1, "Selector case did not execute hardware detection");
            Check(CVar::Lookup("gxBackend")->m_stringValue.m_str == std::string(scenario.backend) &&
                CVar::Lookup("gxAdapter")->m_stringValue.m_str == scenario.adapter, "Recovery overwrote saved selectors");
            Check(CVar::Lookup("gxBackend")->m_defaultValue.m_str == std::string("auto") &&
                CVar::Lookup("gxAdapter")->m_defaultValue.m_str == std::string("auto"), "Early registration lost Auto defaults");
            char archive[8192] = {};
            Check(CVar::LogAll(archive, sizeof(archive), 0, 0), "Selector archive failed");
            for (const auto& preference : std::vector<std::pair<std::string, std::string>>{
                    {"gxBackend", scenario.backend}, {"gxAdapter", scenario.adapter}}) {
                const auto line = "SET " + preference.first + " \"" + preference.second + "\"";
                Check((std::string(archive).find(line) != std::string::npos) == (preference.second != "auto"),
                    "Archive lost saved preference or persisted an environment override");
            }
            CVar::m_needsSave = false; CVar::Destroy();
        }
        unsetenv("WOWDECOMP_GX_BACKEND"); unsetenv("WOWDECOMP_GX_ADAPTER");
        std::remove("window-startup-test.wtf");
        std::cout << "PASS production selector bootstrap: Auto/explicit/unavailable/invalid/missing, environment precedence, saved preservation and repeated restart policy\n";
        SDL_Quit();
        std::cout << "PASS production ConsoleDeviceInitialize/config ingestion: stale modes ignored with hwDetect and hardware-change combinations\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
