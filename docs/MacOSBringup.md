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
  the user confirmed it works. With authentication identity corrected from
  ARM/Mac to x86/Win, the user also confirmed server login and gameplay.
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

## Native Vulkan integration (`vulkan-macos`)

Native Vulkan is kept on a separate branch above the macOS/DXVK checkpoint,
without importing Linux OS providers. It submits directly through MoltenVK.
The Vulkan shader, fixed-function GPU and resident-buffer GPU tests pass with
validation enabled. The GPU fixture now checks partial offscreen viewports and
scissors, not just full-size targets: screen subrectangles are bottom-origin,
whereas texture subrectangles are top-origin, matching the D3D9 backend.

Glow readbacks exposed the importance of that distinction. FFX writes only a
subrectangle of a padded POT texture. Previously its downsample/blur draws landed
at the bottom while subsequent texture reads sampled the top, including magenta
padding. Correcting the offscreen origin puts the blur in the sampled region;
login-scene captures improved. In-world glow still needs user confirmation.

Optional diagnostic captures:

```sh
mkdir -p out/glow-capture
# Add this environment variable to the ordinary native Vulkan launch:
export WOWDECOMP_VULKAN_CAPTURE="$PWD/out/glow-capture"
```

This writes binary PPM files for the first two eligible framebuffer copies and
mixed fixed-vertex/pixel-shader passes with two or four samplers, after 200 draws.
It submits and waits for readback, so leave it disabled for normal gameplay.
`source-*.ppm` contains the pre-copy framebuffer; `pass-<shader-id>-*.ppm`
contains the rendered target, including intentionally unused texture padding.

Normal quit was independently reproduced as exit 134. Debugger traces identified
log admission and thread creation locks destroyed before `Blizzard::Using`'s
cross-translation-unit static shutdown callbacks. Their admission primitives now
have process lifetime; explicit shutdown still joins workers and frees resources.
The real-client normal-SDL-quit smoke now exits 0 with validation enabled. See
`Tools/MacSmoke/README.md` for the opt-in interactive integration test.
