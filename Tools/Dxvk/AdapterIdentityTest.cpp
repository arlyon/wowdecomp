#include "Engine/Source/Gx/CGxDeviceD3d/Native/AdapterIdentity.h"
#include <cassert>
#include <iostream>

struct FakeD3d final : IDirect3D9, ID3D9CpuIdentity1 {
    bool extension = true, badUUID = false, badVersion = false;
    unsigned refs = 1;
    uint32_t driverId = 3;
    uint8_t driverUUID = 77;
    std::vector<uint8_t> order{1, 2};
    HRESULT QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (!extension || iid != __uuidof(ID3D9CpuIdentity1)) return E_NOINTERFACE;
        *out = static_cast<ID3D9CpuIdentity1*>(this); AddRef(); return S_OK;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override { return --refs; }
    HRESULT GetAdapterIdentity(UINT ordinal, D3D9CpuAdapterIdentity1* out) override {
        if (!out || out->size != sizeof(*out) || out->version != 1 || ordinal >= order.size()) return D3DERR_INVALIDCALL;
        *out = {}; out->size = sizeof(*out); out->version = badVersion ? 2 : 1;
        out->deviceUUID[0] = badUUID ? 0 : order[ordinal]; out->driverUUID[0] = driverUUID;
        out->driverId = driverId; out->vendorId = 1002; out->deviceId = 123;
        std::strcpy(out->name, "Duplicate name"); return S_OK;
    }
    UINT GetAdapterCount() override { return order.size(); }
    HRESULT GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9* out) override { std::strcpy(out->Description, "Duplicate name"); return S_OK; }
    HRESULT GetDeviceCaps(UINT ordinal, D3DDEVTYPE, D3DCAPS9*) override { return ordinal < order.size() ? S_OK : D3DERR_INVALIDCALL; }
    HRESULT RegisterSoftwareDevice(void*) override { return E_NOTIMPL; }
    UINT GetAdapterModeCount(UINT, D3DFORMAT) override { return 0; }
    HRESULT EnumAdapterModes(UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*) override { return E_NOTIMPL; }
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE*) override { return E_NOTIMPL; }
    HRESULT CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, WINBOOL) override { return E_NOTIMPL; }
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override { return E_NOTIMPL; }
    HRESULT CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, WINBOOL, D3DMULTISAMPLE_TYPE, DWORD*) override { return E_NOTIMPL; }
    HRESULT CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return E_NOTIMPL; }
    HRESULT CheckDeviceFormatConversion(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override { return E_NOTIMPL; }
    HMONITOR GetAdapterMonitor(UINT) override { return nullptr; }
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**) override { return E_NOTIMPL; }
};
int main() {
    FakeD3d d3d;
    uint8_t uuid[16]{2}; const auto desired = GxAdapterStableId(uuid, 3);
    assert(GxDxvk::SelectOrdinal(d3d, desired) == 1);
    d3d.driverUUID = 88;
    assert(GxDxvk::SelectOrdinal(d3d, desired) == 1); // UUID metadata is not a saved-ID migration.
    d3d.driverUUID = 77; d3d.driverId = 99;
    assert(GxDxvk::SelectOrdinal(d3d, desired) == 0); // Numeric driver-ID change is a genuine missing-ID fallback.
    d3d.driverId = 3;
    std::reverse(d3d.order.begin(), d3d.order.end());
    assert(GxDxvk::SelectOrdinal(d3d, desired) == 0);
    auto records = GxDxvk::EnumerateAdapters(d3d);
    assert(records[0].descriptor.driverUUID[0] == 77 && records[0].descriptor.vendorId == 1002);
    auto descriptors = GxDxvk::Descriptors(records);
    std::reverse(records.begin(), records.end());
    assert(records[GxChooseAdapter(GxDxvk::Descriptors(records), desired)].ordinal == 0);
    assert(GxDxvk::SelectOrdinal(d3d, "bad-uuid") == 0);
    assert(GxDxvk::SelectOrdinal(d3d, GxAdapterStableId(uuid, 99)) == 0);
    d3d.badUUID = true;
    assert(GxDxvk::EnumerateAdapters(d3d)[0].descriptor.id.empty());
    d3d.badUUID = false; d3d.badVersion = true;
    assert(GxDxvk::EnumerateAdapters(d3d)[0].descriptor.id.empty());
    d3d.badVersion = false; d3d.extension = false;
    records = GxDxvk::EnumerateAdapters(d3d);
    assert(records[0].descriptor.id.empty() && records[0].descriptor.reason.find("unavailable") != std::string::npos);
    GxConfigureDeviceSelection("dxvk", desired.c_str());
    GxReportDeviceSelection("dxvk", GxDxvk::Descriptors(records), 0);
    assert(GxGetDeviceSelection().requestedAdapter == desired && GxGetDeviceSelection().activeAdapter.empty());
    assert(GxGetDeviceSelection().warning.find("identity unavailable") != std::string::npos);
    GxReportDeviceSelection("", {}, 0);
    assert(GxGetDeviceSelection().activeBackend.empty() && GxGetDeviceSelection().requestedAdapter == desired);
    assert(d3d.refs == 1);
    d3d.order.clear(); assert(GxDxvk::SelectOrdinal(d3d, desired) == UINT32_MAX);
    std::cout << "DXVK CPU identity: same-instance ordinals, reordered records/ordinals, duplicate names, driver change, invalid UUID/version, old extension, retained preference passed\n";
}
