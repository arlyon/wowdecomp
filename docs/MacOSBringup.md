# macOS bring-up

Stack: `main -> base (#2) -> posix (#5) -> macos (#6)`, parallel to Linux.

## Current client

The native ARM64 Release client builds and renders through **D3D9 -> DXVK Native
-> Vulkan loader -> MoltenVK -> Metal**. The imported OpenGL implementation is
preserved but is not currently selected or qualified. The separate `vulkan-macos`
branch also builds a direct Vulkan/Slang client through MoltenVK, without DXVK;
see the native Vulkan section below.

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

The POSIX cloud-noise fix normalizes samples by `RAND_MAX`, avoiding wrapped
density patterns on non-Windows CRTs. The user confirmed improved in-world
rendering after the authentication and sky/cloud fixes.

No Windows or Intel macOS build has been verified here. Fullscreen/Retina resize,
audio correctness, networking edge cases and unsupported provider behavior still
need broader testing. Selected imported providers and file paths need further
review; a successful build/login screen is not proof of complete gameplay.

## Native Vulkan integration (`vulkan-macos`)

Native Vulkan is kept on a separate branch above the macOS/DXVK checkpoint,
without importing Linux OS providers. It submits directly through MoltenVK.
After building the pinned shader dependencies described in the root README:

```sh
cmake -S . -B out/client-vulkan-mac -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DWOWDECOMP_VULKAN=ON -DWOWDECOMP_DXVK=OFF \
  -DWOWDECOMP_MOJOSHADER_SOURCE="$PWD/out/deps/mojoshader" \
  -DWOWDECOMP_SLANG_SOURCE="$PWD/out/deps/slang" \
  -DWOWDECOMP_SLANGC="$PWD/out/deps/slang-build/Release/bin/slangc"
cmake --build out/client-vulkan-mac --target Wow --parallel 8
```

Both builds write `bin/Mac/Wow`; preserve reference executables or use separate
runtime directories when comparing backends. Use the same loader/ICD environment
as the DXVK launch above, with `WOWDECOMP_GX_BACKEND=vulkan` instead of
`DXVK_WSI_DRIVER`. Add `WOWDECOMP_VULKAN_VALIDATION=1` and the appropriate
`VK_LAYER_PATH` to enable installed validation layers.

The Vulkan shader, fixed-function GPU and resident-buffer GPU tests pass with
validation enabled. The GPU fixture now checks partial offscreen viewports and
scissors, not just full-size targets: screen subrectangles are bottom-origin,
whereas texture subrectangles are top-origin, matching the D3D9 backend.

Glow readbacks exposed the importance of that distinction. FFX writes only a
subrectangle of a padded POT texture. Previously its downsample/blur draws landed
at the bottom while subsequent texture reads sampled the top, including magenta
padding. Correcting the offscreen origin puts the blur in the sampled region;
the user confirmed the pink glow is solved in-world. Temporary readback and
shader-dump instrumentation used to isolate this bug has been removed; the
partial-target GPU regression remains. Offline shader recovery tools are still
available under `Tools/Vulkan`.

Normal quit was independently reproduced as exit 134. Debugger traces identified
log admission and thread creation locks destroyed before `Blizzard::Using`'s
cross-translation-unit static shutdown callbacks. Their admission primitives now
have process lifetime; explicit shutdown still joins workers and frees resources.
The real-client normal-SDL-quit smoke now exits 0 for both rebuilt Vulkan and
DXVK clients (with Vulkan validation enabled for the native backend). See
`Tools/MacSmoke/README.md` for the opt-in interactive integration test.

The platform suite now passes 15/15, including WindowsLeaves, after restoring
the unused original Windows logger/pool leaves. The CPU layout and renderer
selection suite passes 5/5; enable it with `WOWDECOMP_PORTABILITY_TESTS=ON` as
described in the root README.
