#pragma once
#include <d3d9.h>
#include <cstdint>

// Local DXVK extension v1. New ABI revisions require a new IID, never a vtable
// append. No Vulkan declarations or borrowed handles cross this boundary.
struct D3D9CpuAdapterIdentity1 {
    uint32_t size;
    uint32_t version;
    uint8_t deviceUUID[16];
    uint8_t driverUUID[16];
    uint32_t driverId;
    uint32_t vendorId;
    uint32_t deviceId;
    char name[256];
};
static_assert(sizeof(D3D9CpuAdapterIdentity1) == 308, "CPU identity ABI");

MIDL_INTERFACE("7847f6a1-0aca-4fe7-9fd3-e31d927f8b20")
ID3D9CpuIdentity1 : public IUnknown {
    // Exact ordinal of the same IDirect3D9 object, not an independent enumeration.
    // Invalid size/version/ordinal returns D3DERR_INVALIDCALL; no partial result.
    virtual HRESULT STDMETHODCALLTYPE GetAdapterIdentity(
        UINT ordinal, D3D9CpuAdapterIdentity1* identity) = 0;
};
#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D9CpuIdentity1, 0x7847f6a1,0x0aca,0x4fe7,0x9f,0xd3,0xe3,0x1d,0x92,0x7f,0x8b,0x20);
#endif
