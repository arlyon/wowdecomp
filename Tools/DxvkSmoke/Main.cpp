// Real D3D9 device, draw, GPU readback and presentation; no game data required.
#include <SDL.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>

namespace {
bool Check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "%s failed: 0x%08x\n", operation, unsigned(result));
    return false;
}
}

int main() {
    SDL_SetMainReady();
    SDL_setenv("DXVK_WSI_DRIVER", "SDL2", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("WoW DXVK smoke", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 320, 240, SDL_WINDOW_VULKAN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* back = nullptr;
    IDirect3DSurface9* readback = nullptr;
    bool ok = d3d != nullptr;
    if (!ok) std::fprintf(stderr, "Direct3DCreate9 failed\n");
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = 320;
    params.BackBufferHeight = 240;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = reinterpret_cast<HWND>(window);
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (ok) ok = Check(d3d->CreateDevice(0, D3DDEVTYPE_HAL, params.hDeviceWindow,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device), "CreateDevice");
    if (ok) ok = Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff204080, 1.0f, 0), "Clear");
    struct Vertex { float x, y, z, rhw; uint32_t color; };
    const Vertex vertices[] = {
        {40, 30, 0, 1, 0xffff0000}, {280, 30, 0, 1, 0xffff0000},
        {160, 210, 0, 1, 0xffff0000}
    };
    if (ok) ok = Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "SetCullMode");
    if (ok) ok = Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "SetLighting");
    if (ok) ok = Check(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF");
    if (ok) ok = Check(device->BeginScene(), "BeginScene");
    if (ok) {
        ok = Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)), "DrawPrimitiveUP");
        ok = Check(device->EndScene(), "EndScene") && ok;
    }
    if (ok) ok = Check(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back), "GetBackBuffer");
    if (ok) ok = Check(device->CreateOffscreenPlainSurface(320, 240, D3DFMT_X8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), "CreateReadback");
    if (ok) ok = Check(device->GetRenderTargetData(back, readback), "Readback");
    D3DLOCKED_RECT pixels{};
    if (ok) ok = Check(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY), "LockRect");
    if (ok) {
        const auto pixel = [&](int x, int y) {
            return *reinterpret_cast<const uint32_t*>(static_cast<const char*>(pixels.pBits) + y * pixels.Pitch + x * 4) & 0xffffff;
        };
        uint32_t background = pixel(0, 0), triangle = pixel(160, 100);
        ok = background == 0x204080 && triangle == 0xff0000;
        std::printf("Readback: background=%06x triangle=%06x %s\n", background, triangle, ok ? "PASS" : "FAIL");
        ok = Check(readback->UnlockRect(), "UnlockRect") && ok;
    }
    if (ok) ok = Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    if (ok) {
        SDL_Event event;
        for (int frame = 0; frame < 30; ++frame) {
            while (SDL_PollEvent(&event)) {}
            SDL_Delay(16);
        }
        std::puts("D3D9 -> DXVK -> Vulkan: draw/readback/present PASS");
    }
    if (readback) readback->Release();
    if (back) back->Release();
    if (device) device->Release();
    if (d3d) d3d->Release();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
