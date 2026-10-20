#include "NativeVideoOptions.h"
#include "Engine/Source/Gx/DeviceSelection.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "NativeVideoOptionsSource.h"

namespace {
int NativeGraphicsDevices(lua_State* state) {
    const auto& selection = GxGetDeviceSelection();
    const auto text = [&](const char* key, const std::string& value) {
        lua_pushlstring(state, value.data(), value.size()); lua_setfield(state, -2, key);
    };
    const auto boolean = [&](const char* key, bool value) {
        lua_pushboolean(state, value); lua_setfield(state, -2, key);
    };
    lua_createtable(state, 0, 12);
    text("requestedBackend", selection.requestedBackend); text("requestedAdapter", selection.requestedAdapter);
    text("activeBackend", selection.activeBackend); text("activeAdapter", selection.activeAdapter);
    text("activeName", selection.activeName); text("backendOverride", selection.backendOverride);
    text("adapterOverride", selection.adapterOverride); text("loaderFilter", selection.loaderFilter);
    text("warning", selection.warning);
    lua_createtable(state, selection.backends.size(), 0);
    int index = 0;
    for (const auto& backend : selection.backends) {
        lua_createtable(state, 0, 2); text("id", backend.id); text("name", backend.name);
        lua_rawseti(state, -2, ++index);
    }
    lua_setfield(state, -2, "backends");
    lua_createtable(state, selection.adapters.size(), 0);
    index = 0;
    for (const auto& adapter : selection.adapters) {
        lua_createtable(state, 0, 9); text("id", adapter.id); text("name", adapter.name);
        text("driver", adapter.driver); text("reason", adapter.reason);
        boolean("graphics", adapter.graphics); boolean("present", adapter.present);
        boolean("swapchain", adapter.swapchain); boolean("compatible", adapter.compatible);
        lua_pushnumber(state, adapter.driverId); lua_setfield(state, -2, "driverId");
        lua_rawseti(state, -2, ++index);
    }
    lua_setfield(state, -2, "adapters");
    return 1;
}
}

void InstallNativeVideoOptions() {
    // Run only after the original FrameXML has passed its normal signature check.
    FrameScript_RegisterFunction("GetNativeGraphicsDevices", NativeGraphicsDevices);
    FrameScript_Execute(nativeVideoOptionsSource, "NativeVideoOptions", nullptr);
}
