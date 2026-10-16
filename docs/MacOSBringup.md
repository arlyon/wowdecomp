# macOS bring-up

Stack: `main -> base (#2) -> posix (#5) -> macos (#6)`, parallel to Linux.

## Current client

The native ARM64 Release client builds and renders through **D3D9 -> DXVK Native
-> Vulkan loader -> MoltenVK -> Metal**. The imported OpenGL implementation is
preserved but is not currently selected or qualified. Direct Vulkan integration
is the next step; do not confuse it with DXVK's Vulkan implementation.

The port adapts coolzoom/zz's work from
[SatyPardus/wowdecomp#8](https://github.com/SatyPardus/wowdecomp/pull/8), snapshot
`e4297f04bf25c831d30740ee063bbd6127ec37e2`, onto the shared POSIX layer.
Imported [notes](../notes/macos-port.md) describe upstream findings, not validation
of every feature on this branch.

Source selection uses the shared POSIX/SDL providers, Darwin file/debug/thread
leaves, and selected original Mac OS/FM OD runtime leaves. ARM64 CPUID returns
unsupported rather than inventing x86 hardware features. Legacy authentication
wire tags deliberately advertise `x86`/`Win` for 3.3.5a server compatibility;
actual OS/version and CPU reporting remain native. Unsupported security
operations fail explicitly, and archive signature checks remain enabled.

## Build and run

First build the pinned DXVK Native dependency using
[Tools/DxvkSmoke](../Tools/DxvkSmoke/README.md). Also install the client libraries:

```sh
brew install sdl2 libvorbis xvid
cmake -S . -B out/client-mac -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DDXVK_NATIVE_ROOT="$PWD/out/deps/dxvk-mac-prefix" \
  -DCMAKE_BUILD_RPATH="$(brew --prefix)/lib"
cmake --build out/client-mac --target Wow --parallel 8
```

Supply your own 3.3.5a (12340) `Data` under `bin/Mac/Data`, then run:

```sh
cd bin/Mac
SDL_VULKAN_LIBRARY="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib" \
VK_DRIVER_FILES="$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json" \
DXVK_WSI_DRIVER=SDL2 ./Wow
```

Local data and server configuration are not committed. If sharing game assets
with another installation, keep writable runtime configuration separate.

## Evidence and limits

On Apple M1 Pro/macOS 15.5, Apple Clang 17, CMake 4.4.4, Meson 1.12.1 and
Ninja 1.13.2:

- Full ARM64 Release `Wow` executable built and launched with local 12340 data.
- The login screen, including its animated scene, rendered through DXVK/MoltenVK;
  the user confirmed it works. Authentication identity was subsequently corrected
  from ARM/Mac to x86/Win; server login/gameplay still needs revalidation.
- Standalone D3D9 draw/readback/Present passed, with exact background `204080`
  and triangle `ff0000`, using SDL2 2.32.10, MoltenVK package 1.4.2 and Vulkan
  loader 1.4.363.0. Earlier smoke evidence also exists on M4 Max/macOS 26.7.

The latest POSIX cloud-noise fix normalizes samples by `RAND_MAX`, avoiding
wrapped density patterns on non-Windows CRTs. Revalidate sky/cloud rendering in
world after rebuilding with this fix.

No Windows or Intel macOS build has been verified here. Fullscreen/Retina resize,
audio correctness, networking, shutdown and unsupported provider behavior still
need broader testing. Selected imported providers and file paths need further
review; a successful build/login screen is not proof of complete gameplay.
