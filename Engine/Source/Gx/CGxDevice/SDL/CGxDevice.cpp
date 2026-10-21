#include <algorithm>
#include <cmath>
#include <cstring>
#include <SDL.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#if defined(WOWDECOMP_DXVK)
#include "Engine/Source/Gx/CGxDeviceD3d/CGxDeviceD3d.h"
#include "Engine/Source/Gx/CGxDeviceD3d/Native/AdapterIdentity.h"
#endif
#include "Engine/Source/Os/SDL/Window.h"
#if defined(WOWDECOMP_VULKAN)
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#endif

int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    formats.m_chunk = 64;
    formats.Reserve(256, 0);
#if defined(WOWDECOMP_VULKAN)
    if (api == GxApi_Vulkan) return VulkanAdapterFormats(formats);
#endif
#if defined(WOWDECOMP_DXVK)
    return api == GxApi_D3d9 ? CGxDevice::D3dAdapterFormats(formats) : 0;
#else
    return 0;
#endif
}

int32_t CGxDevice::AdjustWindowRectToWorkArea(float aspectRatio, C2iVector* adjust, tagRECT* rect) {
    if (!adjust || !rect) return 0;
    SDL_Rect work;
    if (SDL_GetDisplayUsableBounds(0, &work) != 0) return 0;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;
    const int availableWidth = work.w - adjust->x;
    const int availableHeight = work.h - adjust->y;
    if (width <= 0 || height <= 0 || availableWidth <= 0 || availableHeight <= 0) return 0;
    width = std::min(width, availableWidth);
    height = std::min(height, availableHeight);
    if (aspectRatio > 0.0f && std::isfinite(aspectRatio)) {
        if (width / static_cast<float>(height) > aspectRatio) {
            width = static_cast<int>(height * aspectRatio);
        } else {
            height = static_cast<int>(width / aspectRatio);
        }
    }
    rect->left = work.x + (availableWidth - width) / 2;
    rect->top = work.y + (availableHeight - height) / 2;
    rect->right = rect->left + width;
    rect->bottom = rect->top + height;
    return width > 0 && height > 0;
}

int32_t CGxDevice::DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID,
        uint32_t& driverVersionHi, uint32_t& driverVersionLo) {
#if defined(WOWDECOMP_VULKAN)
    if (GxDefaultApi() == GxApi_Vulkan) return VulkanAdapterID(vendorID, deviceID, driverVersionHi, driverVersionLo);
#endif
    vendorID = deviceID = 0xFFFF;
    driverVersionHi = driverVersionLo = 0;
#if defined(WOWDECOMP_DXVK)
    HINSTANCE library = nullptr;
    LPDIRECT3D9 d3d = nullptr;
    if (!CGxDeviceD3d::ILoadD3dLib(library, d3d)) return 0;
    const UINT ordinal = GxDxvk::SelectOrdinal(*d3d, GxGetDeviceSelection().effectiveAdapter);
    D3DADAPTER_IDENTIFIER9 identifier{};
    const bool valid = SUCCEEDED(d3d->GetAdapterIdentifier(ordinal, 0, &identifier));
    if (valid) {
        vendorID = static_cast<uint16_t>(identifier.VendorId);
        deviceID = static_cast<uint16_t>(identifier.DeviceId);
        driverVersionHi = static_cast<uint32_t>(identifier.DriverVersion.HighPart);
        driverVersionLo = identifier.DriverVersion.LowPart;
    }
    CGxDeviceD3d::IUnloadD3dLib(library, d3d);
    return valid;
#else
    return 0;
#endif
}

int32_t CGxDevice::DeviceAdapterInfer(uint16_t& deviceID) {
#if defined(WOWDECOMP_VULKAN)
    if (GxDefaultApi() == GxApi_Vulkan) {
        uint16_t vendor, adapter; uint32_t high, low;
        if (!VulkanAdapterID(vendor, adapter, high, low)) return 0;
        deviceID = 3; // A Vulkan 1.1 device implements the client's SM3 feature tier.
        return 1;
    }
#endif
    deviceID = 0;
#if defined(WOWDECOMP_DXVK)
    HINSTANCE library = nullptr;
    LPDIRECT3D9 d3d = nullptr;
    if (!CGxDeviceD3d::ILoadD3dLib(library, d3d)) return 0;
    const UINT ordinal = GxDxvk::SelectOrdinal(*d3d, GxGetDeviceSelection().effectiveAdapter);
    D3DCAPS9 caps{};
    const bool valid = SUCCEEDED(d3d->GetDeviceCaps(ordinal, D3DDEVTYPE_HAL, &caps));
    if (valid && (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        if (caps.MaxSimultaneousTextures > 2 && static_cast<uint16_t>(caps.PixelShaderVersion) >= 0x200) deviceID = 3;
        else if (caps.MaxSimultaneousTextures > 2 && static_cast<uint16_t>(caps.PixelShaderVersion) >= 0x101) deviceID = 2;
        else if (caps.MaxSimultaneousTextures >= 2) deviceID = 1;
    }
    CGxDeviceD3d::IUnloadD3dLib(library, d3d);
    return valid;
#else
    return 0;
#endif
}

int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode& mode) {
    SDL_DisplayMode desktop;
    if (SDL_GetDesktopDisplayMode(0, &desktop) != 0) return 0;
    mode.m_size = {desktop.w, desktop.h};
    mode.m_bpp = SDL_BITSPERPIXEL(desktop.format);
    mode.m_refreshRate = desktop.refresh_rate;
    return 1;
}

int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes) {
    modes.SetCount(0);
    const int count = SDL_GetNumDisplayModes(0);
    for (int i = 0; i < count; ++i) {
        SDL_DisplayMode display;
        if (SDL_GetDisplayMode(0, i, &display) != 0 || display.w < 640 || display.h < 480 ||
                SDL_BITSPERPIXEL(display.format) < 16) continue;
        auto mode = modes.New();
        mode->m_size = {display.w, display.h};
        mode->m_bpp = SDL_BITSPERPIXEL(display.format);
        mode->m_refreshRate = display.refresh_rate;
    }
    if (modes.Count() > 1) {
        std::sort(modes.Ptr(), modes.Ptr() + modes.Count(), [](const CGxMonitorMode& a, const CGxMonitorMode& b) {
            return static_cast<int64_t>(a.m_size.x) * a.m_size.y < static_cast<int64_t>(b.m_size.x) * b.m_size.y;
        });
    }
    return modes.Count() != 0;
}

int32_t CGxDevice::D3dAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
#if defined(WOWDECOMP_DXVK)
    HINSTANCE library = nullptr;
    LPDIRECT3D9 d3d = nullptr;
    if (!CGxDeviceD3d::ILoadD3dLib(library, d3d)) return 0;
    const UINT ordinal = GxDxvk::SelectOrdinal(*d3d, GxGetDeviceSelection().effectiveAdapter);
    const D3DFORMAT colors[] = {D3DFMT_R5G6B5, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8};
    const D3DFORMAT depths[] = {D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D24S8, D3DFMT_D32};
    const CGxFormat::Format depthFormats[] = {
        CGxFormat::Fmt_Ds160, CGxFormat::Fmt_Ds24X, CGxFormat::Fmt_Ds248, CGxFormat::Fmt_Ds320
    };
    const D3DMULTISAMPLE_TYPE samples[] = {
        D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_4_SAMPLES,
        D3DMULTISAMPLE_6_SAMPLES, D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_10_SAMPLES,
        D3DMULTISAMPLE_12_SAMPLES, D3DMULTISAMPLE_14_SAMPLES, D3DMULTISAMPLE_16_SAMPLES
    };
    for (D3DFORMAT color : colors) {
        const UINT count = d3d->GetAdapterModeCount(ordinal, color);
        for (UINT i = 0; i < count; ++i) {
            D3DDISPLAYMODE display;
            if (FAILED(d3d->EnumAdapterModes(ordinal, color, i, &display))) continue;
            CGxFormat format;
            std::memset(&format, 0, sizeof(format));
            format.m_size = {static_cast<int32_t>(display.Width), static_cast<int32_t>(display.Height)};
            format.m_refreshRate = display.RefreshRate;
            switch (display.Format) {
                case D3DFMT_R5G6B5: format.m_colorFormat = CGxFormat::Fmt_Rgb565; break;
                case D3DFMT_X8R8G8B8: format.m_colorFormat = CGxFormat::Fmt_ArgbX888; break;
                case D3DFMT_A8R8G8B8: format.m_colorFormat = CGxFormat::Fmt_Argb8888; break;
                default: continue;
            }
            for (size_t depth = 0; depth < 4; ++depth) {
                if (FAILED(d3d->CheckDeviceFormat(ordinal, D3DDEVTYPE_HAL, display.Format,
                        D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, depths[depth]))) continue;
                format.m_depthFormat = depthFormats[depth];
                for (D3DMULTISAMPLE_TYPE sample : samples) {
                    if (FAILED(d3d->CheckDeviceMultiSampleType(ordinal, D3DDEVTYPE_HAL, display.Format, FALSE, sample, nullptr)) ||
                        FAILED(d3d->CheckDeviceMultiSampleType(ordinal, D3DDEVTYPE_HAL, depths[depth], FALSE, sample, nullptr))) continue;
                    format.m_multisampleCount = sample == D3DMULTISAMPLE_NONE ? 1 : static_cast<uint32_t>(sample);
                    if (!CGxDevice::FormatExists(formats, format)) *formats.New() = format;
                }
            }
        }
    }
    CGxDeviceD3d::IUnloadD3dLib(library, d3d);
    return formats.Count() != 0;
#else
    formats.SetCount(0);
    return 0;
#endif
}
