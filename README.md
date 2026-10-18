# wowdecomp

A C++ decompilation of the World of Warcraft 3.3.5a client (`Wow.exe`, build 12340, June 2010).

Join the discord for other clients and to show off some cool stuff you build.

https://discord.gg/DsZMGUD8aT

Every function is reconstructed from the original binary and keeps the address it came from in an
`// OFFSET:` tag, so each one can be compared against the original. The source tree follows the layout
of the original client (`Engine/Source/<Module>`, `WoW/Source/<Module>`, original file names).

x64 is the main target. Win32 still builds and is used to check struct layouts against the 32-bit
original.

## Progress

The percentages below measure how much of the original binary has a counterpart in this source tree. They do
**not** say how much of the game works, and the missing part is not missing gameplay: the client builds into a
complete executable and the game is 100% playable. Bugs could still be hiding, though; a ported function can behave differently
from the original.

Measured against the 12340 binary on 2026-10-15. A function counts as ported when its address is
tagged in the source.

| | Ported | Total | Share of the original |
|---|---:|---:|---:|
| Functions | 24,515 | 27,157 | 90.3% |
| Code bytes | 5,270,186 | 5,656,573 | 93.2% |

The total leaves out 658 functions that IDA identifies as compiler library code and 132 import thunks.
Most of the untagged remainder is code the build gets elsewhere: compiler-generated code (static
initializers, template instances) that our compiler emits on its own, C runtime helpers, and third-party
libraries that are linked from their upstream sources instead of being decompiled (FreeType, zlib, PCRE,
parts of FMOD Ex).

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with the **Desktop development with C++** workload (MSVC v143 and a Windows SDK)
- CMake 3.13 or newer (the one that ships with Visual Studio works)
- For Win32 builds only: [NASM](https://www.nasm.us/), on `PATH` or passed as `-DNASM_EXECUTABLE=<path>`, and
  `DivxDecoder.dll` from your 3.3.5a client copied to `ThirdParty/DivxDecoder/` (the build copies it next to the
  executables; the x64 build uses its own decoder instead)
- A World of Warcraft **3.3.5a (12340)** client. No game data is included in this repository.

## Building

From a Developer PowerShell or any shell with `cmake` on `PATH`:

```powershell
git clone https://github.com/SatyPardus/wowdecomp.git
cd wowdecomp

# x64 (main target)
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target WowClient --parallel

# Win32 (optional, needs NASM)
cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build/win32 --config Release --target WowClient --parallel
```

In-source builds are refused, so keep the build directory separate (`build/` above).

The client is written to `bin/Win/`:

| Target | x64 | Win32 |
|---|---|---|
| `WowClient` | `WowClient64.exe` | `WowClient32.exe` |
| `Wow` | `Wow64.exe` | `Wow32.exe` |

`WowClient` is the client with a crash logger: it opens a console window and writes
`WowClient_crash.log` when the client crashes. `Wow` is the same client without it. You can also turn the
logger off in `WowClient` with `-DWOWCLIENT_CRASH_LOG=OFF`.

### Options

| CMake option | Default | Effect |
|---|---|---|
| `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` | `OFF` | Skip the MPQ file and `.toc.sig` signature checks, so modified game data and interface files load |
| `WOWCLIENT_CRASH_LOG` | `ON` | Build `WowClient` with the crash logger |
| `WOWDECOMP_VULKAN` | `OFF` | Build the experimental native Vulkan/Slang backend on Linux |
| `WOWDECOMP_DXVK` | `ON` | Build the native D3D9/DXVK backend; disable for a Vulkan-only client |
| `WOWCLIENT_OUTPUT_OVERRIDE` | empty | Name the `WowClient` executable something other than `WowClient64` / `WowClient32` |

### Platform foundation and macOS handoff

See [Platform portability](docs/PlatformPortability.md) for target OS/CPU/layout
selection, shared SDL/POSIX layers, and the remaining Darwin and ARM64 integration
inventory. macOS client builds are not enabled yet.

### Native Vulkan (Linux bring-up)

This opt-in backend uses the existing Gx interface and submits Vulkan commands directly. It is
currently a **raster baseline**, not a ray tracer or a performance improvement claim. SDL2, Vulkan
headers/loader and the shader dependencies below are required. A DXVK-native development prefix is
required only with `WOWDECOMP_DXVK=ON`; use `-DWOWDECOMP_DXVK=OFF` for a Vulkan-only build.
Windows Vulkan and macOS/MoltenVK have not been built or run.

Dependencies are resolved locally through Conan, not installed system-wide, and are pinned by the
recipes in `conan/`. The CMake integration checks these revisions:

- [MojoShader](https://github.com/icculus/mojoshader), `ad5dff84830c2863c841f4b1f4e3df78c705b383` (zlib).
- [Slang v2026.4.2](https://github.com/shader-slang/slang/tree/v2026.4.2),
  `5cde9ea5ff3b6c657978de54d2241a86cb3528b1` (MIT/Apache-2.0; see its third-party licences).

`make deps` clones the pinned revisions into the local Conan cache, builds `slangc` only when it is
not cached, and writes `out/conan/deps.cmake`, which the client preset injects with
`CMAKE_PROJECT_INCLUDE`:

```sh
make deps     # conan create conan/recipes/{mojoshader,slang} --build=missing + conan install
make client   # configure + build out/vulkan-client/bin/Wow
make run      # launch against GAME_DATA
```

See [conan/README.md](conan/README.md) for the recipes and the cache-variable handoff. A hand
configuration (for example a DXVK-enabled build) takes the same variables through that file:

```sh
cmake -S . -B out/client-dxvk -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DWOWDECOMP_VULKAN=ON -DWOWDECOMP_DXVK=ON -DDXVK_NATIVE_ROOT=/path/to/dxvk-native/prefix \
  -DCMAKE_PROJECT_INCLUDE="$PWD/out/conan/deps.cmake"
cmake --build out/client-dxvk --target Wow --parallel 4
```

The client finds game data beside its executable. For isolated comparisons, copy the executable into
separate run directories, link each directory's `Data` to your existing client data, and give each its
own `WTF` and `XDG_CONFIG_HOME`. Set `WOWDECOMP_GX_BACKEND=vulkan` (the enabled-build default) or
`WOWDECOMP_GX_BACKEND=dxvk` when launching, if that backend was built. Unknown or unbuilt backend
selections fail explicitly; building with both backends disabled is also rejected.
`WOWDECOMP_VULKAN_VALIDATION=1` requests
`VK_LAYER_KHRONOS_validation`; supply a local layer through `VK_LAYER_PATH` if it is not installed.
Do not disable archive signature checks merely to exercise a backend.

Authored shaders live in `Engine/Source/Gx/CGxDeviceVulkan/Shaders/*.slang`. Existing D3D9 shader
bytecode is recovered mechanically into Slang, then compiled to SPIR-V; this is not original source.
`Tools/Vulkan` builds standalone synthetic translation/compilation tests and `VulkanShaderSource`,
which accepts a GXSH `.bls` and an output directory. No proprietary shader data is checked in.

```sh
make tests    # configure + build out/vulkan-tests against the Conan deps, then ctest
```

The same dependencies are handed to hand-configured builds with
`-DCMAKE_PROJECT_INCLUDE="$PWD/out/conan/deps.cmake"`.

Known boundaries: shader-model 1.4 translation is unsupported, and some shader-model 1.1 pixel inputs
are not recovered correctly. The audited model 2/3 variants compile and validate, but offline compilation
does not prove rendering parity. Occlusion queries, hardware cursors, vendor stereo, multisampling and
performance optimisation are not part of this initial backend. Rectangle-coordinate textures and
cubemap render-target planes are also unsupported. Runtime verification is scoped to the intro and
animated login/EULA scene, not authenticated gameplay or every world rendering path.

The Linux JPEG screenshot writer currently crashes in both DXVK and native Vulkan (in
`rgb_ycc_start`, independently of rendering). Use `SET screenshotFormat "tga"` in the run directory's
`WTF/Config.wtf`, or capture the client window externally. TGA readback works with both backends.

### Linux portability regression tests

The native OS and maths tests are independent of the client build and need no game data, Vulkan,
or working display. They require a C++17 compiler, CMake 3.13 or newer, pkg-config and SDL2 development
files (2.0.22 or newer, for extended text-event ownership). Input tests use SDL's dummy video driver
without touching the desktop:

```sh
cmake -S Tools/Portability -B out/portability-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/portability-tests --parallel
ctest --test-dir out/portability-tests --output-on-failure
```

Optionally set `WOWDECOMP_MSVC_BASE` to an explicit Git revision to register the preprocessing
preservation check in CTest (requires Python 3). This checks active MSVC-macro source, not Windows
binary matching or decompilation coverage. These test targets do not build or launch the game.

### Adding platforms

See [Platform boundaries](docs/PlatformBoundaries.md) for the additive provider
contracts, architecture selection, and Darwin integration map. Windows keeps its
existing implementations. Reusable POSIX providers and OS integrations live in
the subsequent stack layers.

## Running

1. Copy the `Data` folder of your 3.3.5a client into `bin/Win/`, so you have `bin/Win/Data/`.
2. Point `Data/<locale>/realmlist.wtf` at the server you want to connect to.
3. Start `bin/Win/WowClient64.exe` (or `Wow64.exe`).

## Layout

| Path | Contents |
|---|---|
| `Engine/Source/` | Engine modules: Storm, Gx (Direct3D 9 / OpenGL), Model2, Frame/FrameXML UI, SoundEngine, Lua, ... |
| `WoW/Source/` | Game modules: objects, world rendering, UI, networking, database, glue screens |
| `WoW/Common/` | Code shared by the game modules |
| `ThirdParty/` | Libraries linked from their upstream sources |
| `Tools/WowClient/` | The `WowClient` executable target |
| `Tools/XvidDecoder/` | x64 movie decoder that replaces the original 32-bit `DivxDecoder.dll` |

## Special thanks

- [binana](https://github.com/thunderbrewhq/binana)
- [thunderbrew](https://github.com/thunderbrewhq/thunderbrew)
- [whoa](https://github.com/whoahq/whoa)
- TOMRUS, for the initial IDB
- Everyone who has reported bugs so far
- The WoW modding community
