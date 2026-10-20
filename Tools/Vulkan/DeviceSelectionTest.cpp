#include "Engine/Source/Gx/DeviceSelection.h"
#include <cassert>
#include <iostream>
#include <cstdlib>

int main() {
    uint8_t a[16] = {1}, b[16] = {2};
    const auto first = GxAdapterStableId(a, 3), second = GxAdapterStableId(b, 3);
    assert(first == "01000000000000000000000000000000:3");
    assert(GxValidAdapterId(first) && GxValidAdapterId("auto"));
    for (const char* invalid : {"", "0", "../driver.so", "AUTO", "01000000000000000000000000000000:03",
        "01000000000000000000000000000000:4294967296", "0100000000000000000000000000000g:3"})
        assert(!GxValidAdapterId(invalid));
    assert(GxValidAdapterId("01000000000000000000000000000000:4294967295"));
    GxAdapterDescriptor capability;
    capability.graphics = capability.present = capability.swapchain = true;
    assert(capability.IsPresentationCompatible(true, true, true));
    capability.present = false;
    assert(!capability.IsPresentationCompatible(true, true, true));
    capability.present = true; capability.swapchain = false;
    assert(!capability.IsPresentationCompatible(true, true, true));
    capability.swapchain = true;
    assert(!capability.IsPresentationCompatible(false, true, true));
    assert(!capability.IsPresentationCompatible(true, false, true));
    assert(!capability.IsPresentationCompatible(true, true, false));
    std::vector<GxAdapterDescriptor> devices(2);
    devices[0].id = first; devices[1].id = second;
    for (auto& d : devices) { d.name = "Same GPU name"; d.compatible = true; }
    assert(GxChooseAdapter(devices, second) == 1);
    std::reverse(devices.begin(), devices.end());
    assert(GxChooseAdapter(devices, second) == 0);
    devices[0].compatible = false; devices[0].reason = "No present queue";
    assert(GxChooseAdapter(devices, second) == 1);
    assert(GxChooseAdapter(devices, "auto") == 1);
    assert(GxChooseAdapter(devices, "invalid") == 1);
    devices[1].compatible = false; devices[1].reason = "Swapchain extension missing";
    assert(GxChooseAdapter(devices, first) == devices.size());
    std::vector<GxBackendDescriptor> backends{{"vulkan", "Native Vulkan"}};
    auto s = GxResolveSelection(backends, "dxvk", second, "", "");
    assert(s.effectiveBackend == "vulkan" && s.requestedBackend == "dxvk" && !s.warning.empty());
    s = GxResolveSelection(backends, "auto", first, "vulkan", second);
    assert(s.effectiveBackend == "vulkan" && s.effectiveAdapter == second && s.requestedAdapter == first);
    s = GxResolveSelection(backends, "auto", "invalid", "", "");
    assert(s.effectiveAdapter == "auto" && s.requestedAdapter == "invalid" && !s.warning.empty());
    s = GxResolveSelection(backends, "auto", "auto", "../../module.so", "");
    assert(s.effectiveBackend == "vulkan" && !s.warning.empty());
    s = GxResolveSelection({}, "auto", "auto", "", "");
    assert(s.effectiveBackend.empty() && !s.warning.empty());
    setenv("WOWDECOMP_GX_BACKEND", "vulkan", 1);
    setenv("WOWDECOMP_GX_ADAPTER", second.c_str(), 1);
    GxConfigureDeviceSelection("auto", first.c_str());
    assert(GxGetDeviceSelection().backends.size() == 1 && GxGetDeviceSelection().backends[0].id == "vulkan");
    assert(GxGetDeviceSelection().requestedAdapter == first && GxGetDeviceSelection().effectiveAdapter == second);
    unsetenv("WOWDECOMP_GX_BACKEND"); unsetenv("WOWDECOMP_GX_ADAPTER");
    GxConfigureDeviceSelection("vulkan", second.c_str());
    devices[0].compatible = false; devices[1].compatible = true;
    GxReportDeviceSelection("vulkan", devices, GxChooseAdapter(devices, second));
    const auto& active = GxGetDeviceSelection();
    assert(active.requestedAdapter == second && active.activeAdapter == first && !active.warning.empty());
    const std::string missing = "ffffffffffffffffffffffffffffffff:3";
    GxConfigureDeviceSelection("vulkan", missing.c_str());
    GxReportDeviceSelection("vulkan", devices, GxChooseAdapter(devices, missing));
    assert(GxGetDeviceSelection().requestedAdapter == missing && GxGetDeviceSelection().activeAdapter == first);
    assert(!GxGetDeviceSelection().warning.empty());
    std::cout << "DeviceSelection: stable IDs, reversed order, duplicate names, compatibility, Auto, invalid IDs, precedence and saved-desired recovery passed\n";
}
