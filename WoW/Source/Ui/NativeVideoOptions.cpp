#include "NativeVideoOptions.h"
#include "Engine/Source/Gx/DeviceSelection.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "NativeVideoOptionsSource.h"

namespace {
int NativeReloadHdrLut(lua_State*) {
    if (g_theGxDevicePtr && CWorldParam::cvar_nativeHDRLut)
        g_theGxDevicePtr->SceneRequestHdrLut(CWorldParam::cvar_nativeHDRLut->m_stringValue.m_str, true);
    return 0;
}
int NativeHdrStatus(lua_State* state) {
    // Owner-thread polling only; file opening/parsing stays in the owned job.
    if (g_theGxDevicePtr && CWorldParam::cvar_nativeHDRLut)
        g_theGxDevicePtr->SceneRequestHdrLut(CWorldParam::cvar_nativeHDRLut->m_stringValue.m_str);
    lua_createtable(state, 0, 3);
    lua_pushboolean(state, g_theGxDevicePtr && g_theGxDevicePtr->SceneHdrSupported()); lua_setfield(state, -2, "supported");
    // Instantaneous scope state, not a last-completed-frame claim.
    lua_pushboolean(state, g_theGxDevicePtr && g_theGxDevicePtr->SceneHdrActive()); lua_setfield(state, -2, "active");
    lua_pushstring(state, g_theGxDevicePtr ? g_theGxDevicePtr->SceneHdrLutStatus() : "No graphics device"); lua_setfield(state, -2, "lut");
    return 1;
}
int NativeShadowStatus(lua_State* state) {
    const auto status=g_theGxDevicePtr?g_theGxDevicePtr->SceneShadowStatus():GxShadow::Status{};
    const char* message="Invalid status";
    switch(status.result){
    case GxShadow::FrameResult::Disabled:message="Off (original raster)";break;
    case GxShadow::FrameResult::Ready:message=status.terrainRendered?"Running: shadows on terrain":status.diagnosticRendered?"Running: diagnostic visibility mask":"Geometry ready; waiting for receivers";break;
    case GxShadow::FrameResult::Unavailable:message="Not running: required terrain settings or renderer features unavailable";break;
    case GxShadow::FrameResult::PendingGeometry:message="Waiting for loaded world geometry";break;
    case GxShadow::FrameResult::PendingTexture:message="Waiting for original coverage textures";break;
    case GxShadow::FrameResult::StalePose:message="Current caster pose unavailable";break;
    case GxShadow::FrameResult::InvalidGeometry:message="Invalid caster geometry";break;
    case GxShadow::FrameResult::UnsupportedCoverage:message="Not running: unsupported caster or receiver material; original rendering retained";break;
    case GxShadow::FrameResult::AllocationFailed:message="Ray resource allocation/recording failed; raster retained";break;
    }
    lua_createtable(state,0,7);
    lua_pushboolean(state,g_theGxDevicePtr&&g_theGxDevicePtr->SceneShadowsSupported());lua_setfield(state,-2,"supported");
    lua_pushboolean(state,status.diagnosticRendered);lua_setfield(state,-2,"rendered");
    const std::string description=status.detail.empty()?message:std::string("Not running: ")+status.detail;
    lua_pushlstring(state,description.data(),description.size());lua_setfield(state,-2,"status");
    lua_pushboolean(state,status.terrainRendered);lua_setfield(state,-2,"terrainRendered");
    lua_pushnumber(state,status.terrainReceivers);lua_setfield(state,-2,"receivers");
    lua_pushnumber(state,status.triangles);lua_setfield(state,-2,"triangles");
    return 1;
}
int NativeIndependentWorldSupported(lua_State* state) {
    lua_pushboolean(state,g_theGxDevicePtr && g_theGxDevicePtr->SceneIndependentWorldSupported());return 1;
}
int NativeIndependentWorldStatus(lua_State* state) {
    lua_pushstring(state,g_theGxDevicePtr?g_theGxDevicePtr->SceneIndependentWorldStatus():"No graphics device");return 1;
}
int NativeTemporalSupported(lua_State* state) {
    lua_pushboolean(state, g_theGxDevicePtr && g_theGxDevicePtr->SceneTemporalSupported());
    return 1;
}
int NativeSceneScaleSupported(lua_State* state) {
    lua_pushboolean(state, g_theGxDevicePtr && g_theGxDevicePtr->SceneRenderScaleSupported());
    return 1;
}
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
    FrameScript_RegisterFunction("ReloadNativeHdrLut", NativeReloadHdrLut);
    FrameScript_RegisterFunction("GetNativeHdrStatus", NativeHdrStatus);
    FrameScript_RegisterFunction("GetNativeShadowStatus", NativeShadowStatus);
    FrameScript_RegisterFunction("GetNativeGraphicsDevices", NativeGraphicsDevices);
    FrameScript_RegisterFunction("IsNativeIndependentWorldSupported",NativeIndependentWorldSupported);
    FrameScript_RegisterFunction("GetNativeIndependentWorldStatus",NativeIndependentWorldStatus);
    FrameScript_RegisterFunction("IsNativeTemporalAASupported", NativeTemporalSupported);
    FrameScript_RegisterFunction("IsNativeSceneScalingSupported", NativeSceneScaleSupported);
    FrameScript_Execute(nativeVideoOptionsSource, "NativeVideoOptions", nullptr);
}
