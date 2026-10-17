#include "Engine/Source/Gx/Gx.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
void Select(const char* name, bool available, EGxApi api) {
    Check(setenv("WOWDECOMP_GX_BACKEND", name, 1) == 0, "set backend selection");
    bool rejected = false;
    try { Check(GxDefaultApi() == api, "requested backend is selected exactly"); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected == !available, "unbuilt or invalid backend is rejected");
}
}
int main() {
#if defined(WOWDECOMP_DXVK)
    constexpr bool dxvk = true;
#else
    constexpr bool dxvk = false;
#endif
#if defined(WOWDECOMP_VULKAN)
    constexpr bool vulkan = true;
#else
    constexpr bool vulkan = false;
#endif
    constexpr auto vulkanApi = static_cast<EGxApi>(6);
    Check(unsetenv("WOWDECOMP_GX_BACKEND") == 0, "clear backend selection");
    Check(GxDefaultApi() == (vulkan ? vulkanApi : GxApi_D3d9), "default is an available backend");
    Check(bool(GxApiSupported(GxApi_D3d9)) == dxvk, "D3D9 availability matches build");
    Check(bool(GxApiSupported(vulkanApi)) == vulkan, "Vulkan availability matches build");
    Check(!GxApiSupported(GxApi_OpenGl) && !GxApiSupported(GxApi_D3d9Ex), "unbuilt legacy APIs are unavailable");
    Check(!GxApiSupported(static_cast<EGxApi>(999)), "unknown API is unavailable");
    Select("dxvk", dxvk, GxApi_D3d9);
    Select("vulkan", vulkan, vulkanApi);
    Select("metal", false, GxApi_D3d9);
    Select("", false, GxApi_D3d9);
    Check(unsetenv("WOWDECOMP_GX_BACKEND") == 0, "restore backend environment");
    return 0;
}
