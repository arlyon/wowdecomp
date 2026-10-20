#include "DeviceSelection.h"
#include <cstdlib>

namespace {
GxDeviceSelection selection;
std::string BoundedText(const char* value, size_t maximum) {
    if (!value) return {};
    size_t size = 0;
    while (size < maximum && value[size]) ++size;
    return std::string(value, size);
}
std::string Environment(const char* key) { return BoundedText(std::getenv(key), 1024); }
}
const GxDeviceSelection& GxGetDeviceSelection() { return selection; }
void GxConfigureDeviceSelection(const char* backend, const char* adapter) {
    std::vector<GxBackendDescriptor> backends;
#if defined(WOWDECOMP_VULKAN)
    backends.push_back({"vulkan", "Native Vulkan"});
#endif
#if defined(WOWDECOMP_DXVK)
    backends.push_back({"dxvk", "D3D9 / DXVK"});
#endif
    selection = GxResolveSelection(std::move(backends), backend ? BoundedText(backend, 64) : "auto",
        adapter ? BoundedText(adapter, 64) : "auto", Environment("WOWDECOMP_GX_BACKEND"), Environment("WOWDECOMP_GX_ADAPTER"));
    for (const char* key : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE"}) {
        auto value = Environment(key);
        if (!value.empty()) selection.loaderFilter += std::string(key) + "=" + value + " ";
    }
}
void GxReportDeviceSelection(const char* activeBackend, std::vector<GxAdapterDescriptor> adapters, size_t selected) {
    selection.activeBackend.clear(); selection.activeAdapter.clear(); selection.activeName.clear();
    selection.adapters = std::move(adapters);
    if (selected >= selection.adapters.size()) return;
    const auto& active = selection.adapters[selected];
    selection.activeBackend = activeBackend;
    selection.activeAdapter = active.id; selection.activeName = active.name;
    if (active.id.empty())
        selection.warning += "Stable adapter identity unavailable; D3D ordinal fallback active. Saved preference retained. ";
    if (selection.effectiveAdapter != "auto" && selection.effectiveAdapter != active.id)
        selection.warning += "Requested adapter missing or incompatible; Auto recovery active. Saved preference retained. ";
}
