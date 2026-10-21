#pragma once
#include "Engine/Source/Gx/DeviceSelection.h"
#include "ThirdParty/dxvk-identity/d3d9_cpu_identity.h"
#include <cstring>
#include <memory>

namespace GxDxvk {
struct Adapter {
    UINT ordinal = 0;
    GxAdapterDescriptor descriptor;
};
// All records belong to this retained caller-owned COM instance. Missing CPU
// extension permits ordinary D3D fallback, but never fabricates a stable ID.
inline std::vector<Adapter> EnumerateAdapters(IDirect3D9& d3d) {
    ID3D9CpuIdentity1* identity = nullptr;
    const HRESULT query = d3d.QueryInterface(__uuidof(ID3D9CpuIdentity1), reinterpret_cast<void**>(&identity));
    const auto release = [](ID3D9CpuIdentity1* p) { if (p) p->Release(); };
    std::unique_ptr<ID3D9CpuIdentity1, decltype(release)> retained(identity, release);
    std::vector<Adapter> adapters;
    for (UINT ordinal = 0; ordinal < d3d.GetAdapterCount(); ++ordinal) {
        Adapter adapter; adapter.ordinal = ordinal;
        auto& d = adapter.descriptor;
        D3DADAPTER_IDENTIFIER9 identifier{};
        if (SUCCEEDED(d3d.GetAdapterIdentifier(ordinal, 0, &identifier)))
            d.name.assign(identifier.Description, strnlen(identifier.Description, sizeof(identifier.Description)));
        D3DCAPS9 caps{};
        d.compatible = d.graphics = SUCCEEDED(d3d.GetDeviceCaps(ordinal, D3DDEVTYPE_HAL, &caps));
        d.reason = d.compatible ? "D3D caps available; presentation unverified until CreateDevice" : "D3D device caps unavailable";
        D3D9CpuAdapterIdentity1 value{}; value.size = sizeof(value); value.version = 1;
        if (SUCCEEDED(query) && identity && SUCCEEDED(identity->GetAdapterIdentity(ordinal, &value)) &&
            value.size == sizeof(value) && value.version == 1 &&
            std::any_of(std::begin(value.deviceUUID), std::end(value.deviceUUID), [](uint8_t b) { return b != 0; })) {
            d.id = GxAdapterStableId(value.deviceUUID, value.driverId);
            d.driverId = value.driverId;
            std::copy(std::begin(value.driverUUID), std::end(value.driverUUID), d.driverUUID.begin());
            d.vendorId = value.vendorId; d.deviceId = value.deviceId;
            d.name.assign(value.name, strnlen(value.name, sizeof(value.name)));
            d.driver = "DXVK CPU identity v1";
        } else {
            d.reason += "; stable identity unavailable (DXVK CPU extension v1 missing or invalid)";
        }
        adapters.push_back(std::move(adapter));
    }
    return adapters;
}
inline std::vector<GxAdapterDescriptor> Descriptors(const std::vector<Adapter>& adapters) {
    std::vector<GxAdapterDescriptor> result;
    for (const auto& adapter : adapters) result.push_back(adapter.descriptor);
    return result;
}
inline UINT SelectOrdinal(IDirect3D9& d3d, const std::string& desired) {
    const auto adapters = EnumerateAdapters(d3d);
    const size_t selected = GxChooseAdapter(Descriptors(adapters), desired);
    return selected < adapters.size() ? adapters[selected].ordinal : UINT32_MAX;
}
}
