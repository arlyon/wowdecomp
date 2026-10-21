#include "Engine/Source/Gx/CGxDeviceD3d/Native/AdapterIdentity.h"
#include <SDL.h>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <sstream>

int main(int argc, char** argv) {
    const bool old = argc == 2 && std::string(argv[1]) == "--old-extension";
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window* window = SDL_CreateWindow("DXVK identity fixture", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, 64, 64, SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    assert(window);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    assert(d3d);
    ID3D9CpuIdentity1* cpu = nullptr;
    const HRESULT query = d3d->QueryInterface(__uuidof(ID3D9CpuIdentity1), reinterpret_cast<void**>(&cpu));
    if (old) {
        assert(query == E_NOINTERFACE && !cpu);
        const auto adapters = GxDxvk::EnumerateAdapters(*d3d);
        assert(!adapters.empty() && adapters[0].descriptor.id.empty());
        std::cout << "Old dependency: extension unavailable, no fabricated stable ID\n";
    } else {
        assert(SUCCEEDED(query) && cpu);
        assert(cpu->GetAdapterIdentity(0, nullptr) == D3DERR_INVALIDCALL);
        D3D9CpuAdapterIdentity1 invalid{};
        assert(cpu->GetAdapterIdentity(0, &invalid) == D3DERR_INVALIDCALL);
        invalid.size = sizeof(invalid); invalid.version = 2;
        assert(cpu->GetAdapterIdentity(0, &invalid) == D3DERR_INVALIDCALL);
        invalid.version = 1;
        assert(cpu->GetAdapterIdentity(d3d->GetAdapterCount(), &invalid) == D3DERR_INVALIDCALL);
        const auto adapters = GxDxvk::EnumerateAdapters(*d3d);
        assert(!adapters.empty());
        for (const auto& adapter : adapters) {
            assert(!adapter.descriptor.id.empty());
            const UINT ordinal = GxDxvk::SelectOrdinal(*d3d, adapter.descriptor.id);
            assert(ordinal == adapter.ordinal);
            D3DPRESENT_PARAMETERS parameters{};
            parameters.BackBufferWidth = parameters.BackBufferHeight = 64;
            parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
            parameters.BackBufferCount = 1;
            parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
            parameters.hDeviceWindow = reinterpret_cast<HWND>(window);
            parameters.Windowed = TRUE;
            parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            IDirect3DDevice9* device = nullptr;
            assert(SUCCEEDED(d3d->CreateDevice(ordinal, D3DDEVTYPE_HAL, reinterpret_cast<HWND>(window),
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device)) && device);
            D3DDEVICE_CREATION_PARAMETERS created{};
            assert(SUCCEEDED(device->GetCreationParameters(&created)) && created.AdapterOrdinal == ordinal);
            IDirect3D9* owner = nullptr;
            assert(SUCCEEDED(device->GetDirect3D(&owner)) && owner == d3d);
            const auto actual = GxDxvk::EnumerateAdapters(*owner);
            assert(actual[created.AdapterOrdinal].descriptor.id == adapter.descriptor.id);
            assert(actual[created.AdapterOrdinal].descriptor.driverUUID == adapter.descriptor.driverUUID);
            std::ostringstream driverUUID;
            for (uint8_t b : adapter.descriptor.driverUUID)
                driverUUID << std::hex << std::setfill('0') << std::setw(2) << unsigned(b);
            std::cout << "driverUUID=" << driverUUID.str() << " vendorId=" << adapter.descriptor.vendorId
                << " deviceId=" << adapter.descriptor.deviceId << " driverId=" << adapter.descriptor.driverId << '\n';
            std::cout << "requested=" << adapter.descriptor.id << " actual=" << actual[created.AdapterOrdinal].descriptor.id
                << " ordinal=" << ordinal << " same-COM-instance=true name=" << adapter.descriptor.name << '\n';
            owner->Release(); device->Release();
        }
        uint8_t missing[16]; std::fill(std::begin(missing), std::end(missing), 255);
        assert(GxDxvk::SelectOrdinal(*d3d, GxAdapterStableId(missing, UINT32_MAX)) == adapters.front().ordinal);
        std::cout << "Physical adapter count=" << adapters.size() << "; multi-GPU proof=" << (adapters.size() > 1 ? "yes" : "unavailable") << '\n';
        cpu->Release();
    }
    d3d->Release(); SDL_DestroyWindow(window); SDL_Quit();
}
