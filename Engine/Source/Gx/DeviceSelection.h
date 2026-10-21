#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// CPU-only values: Vulkan handles never cross the Gx boundary.
struct GxBackendDescriptor { std::string id, name; };
struct GxAdapterDescriptor {
    std::string id, name, driver, reason;
    uint32_t driverId = 0, vendorId = 0, deviceId = 0;
    std::array<uint8_t, 16> driverUUID{};
    bool graphics = false, present = false, swapchain = false, compatible = false;
    bool IsPresentationCompatible(bool requiredApiSupported, bool surfaceFormatsAvailable, bool presentModesAvailable) const {
        return requiredApiSupported && graphics && present && swapchain && surfaceFormatsAvailable && presentModesAvailable;
    }
};
struct GxDeviceSelection {
    std::vector<GxBackendDescriptor> backends;
    std::vector<GxAdapterDescriptor> adapters;
    std::string requestedBackend = "auto", requestedAdapter = "auto";
    std::string effectiveBackend, effectiveAdapter = "auto", activeBackend, activeAdapter, activeName;
    std::string backendOverride, adapterOverride, loaderFilter, warning;
};
inline bool GxValidBackendId(const std::string& id) {
    return id == "auto" || id == "vulkan" || id == "dxvk";
}
inline bool GxValidAdapterId(const std::string& id) {
    if (id == "auto") return true;
    if (id.size() < 34 || id.size() > 43 || id[32] != ':') return false;
    for (size_t i = 0; i < 32; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    if (id.size() > 34 && id[33] == '0') return false;
    uint64_t driver = 0;
    for (size_t i = 33; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
        driver = driver * 10 + id[i] - '0';
        if (driver > UINT32_MAX) return false;
    }
    return true;
}
inline std::string GxAdapterStableId(const uint8_t* uuid, uint32_t driver) {
    static const char hex[] = "0123456789abcdef";
    std::string id;
    for (unsigned i = 0; i < 16; ++i) { id += hex[uuid[i] >> 4]; id += hex[uuid[i] & 15]; }
    return id + ':' + std::to_string(driver);
}
inline size_t GxChooseAdapter(const std::vector<GxAdapterDescriptor>& adapters, const std::string& desired) {
    if (GxValidAdapterId(desired) && desired != "auto")
        for (size_t i = 0; i < adapters.size(); ++i)
            if (adapters[i].compatible && adapters[i].id == desired) return i;
    for (size_t i = 0; i < adapters.size(); ++i) if (adapters[i].compatible) return i;
    return adapters.size();
}
inline GxDeviceSelection GxResolveSelection(std::vector<GxBackendDescriptor> backends,
    const std::string& backend, const std::string& adapter, const std::string& backendOverride,
    const std::string& adapterOverride) {
    GxDeviceSelection s;
    s.backends = std::move(backends); s.requestedBackend = backend; s.requestedAdapter = adapter;
    s.backendOverride = backendOverride; s.adapterOverride = adapterOverride;
    s.effectiveBackend = backendOverride.empty() ? backend : backendOverride;
    s.effectiveAdapter = adapterOverride.empty() ? adapter : adapterOverride;
    if (s.backends.empty()) { s.effectiveBackend.clear(); s.warning = "No compiled renderer available"; return s; }
    if (s.effectiveBackend == "auto") s.effectiveBackend = s.backends.front().id;
    if (!GxValidBackendId(s.effectiveBackend) || std::none_of(s.backends.begin(), s.backends.end(),
        [&](const auto& b) { return b.id == s.effectiveBackend; })) {
        s.warning = "Requested renderer unavailable; Auto recovery active. Saved preference retained. ";
        s.effectiveBackend = s.backends.front().id;
    }
    if (!GxValidAdapterId(s.effectiveAdapter)) {
        s.warning += "Invalid adapter ID; Auto recovery active. Saved preference retained. ";
        s.effectiveAdapter = "auto";
    }
    return s;
}
const GxDeviceSelection& GxGetDeviceSelection();
void GxConfigureDeviceSelection(const char* backend, const char* adapter);
void GxReportDeviceSelection(const char* activeBackend, std::vector<GxAdapterDescriptor> adapters, size_t selected);
