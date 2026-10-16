# Native platform integration and macOS handoff

## Status and branch stack

`platform-foundation` adds target identity above `main`. `linux-native` is rebased
onto that foundation and separates reusable native code from Linux providers.
`native-vulkan` is stacked above Linux. Original tips are retained as
`backup/linux-native-before-platform` and `backup/native-vulkan-before-platform`.
Nothing in this work establishes a successful macOS or Windows build.

The client still builds on Linux x86-64 and Windows x86/x86-64. Darwin and ARM64
client configuration deliberately fails with an implementation-required error.
The standalone platform tests recognize those targets without claiming their
client implementations exist. Build each macOS architecture separately; universal
binaries are deferred.

## Target identity and source ownership

- `cmake/WowPlatform.cmake` uses **target**, not host, identity. It separates
  `WOWDECOMP_OS` (`Windows`, `Linux`, `Darwin`), `WOWDECOMP_CPU` (`x86`, `x86_64`,
  `arm64`), `WOWDECOMP_POINTER_BITS`, and `WOWDECOMP_COMPILER`. Explicit Apple
  architectures and Visual Studio target selection take precedence over host CPU.
- `Engine/Source/Platform/Platform.h` provides zero-or-one OS/CPU/compiler macros
  from compiler target definitions for standalone source compilation.
- Existing `WOWDECOMP_ARCH_32/64` macros continue to mean **layout/pointer width**.
  They do not mean Windows or an x86 instruction set. `_MSC_VER` still identifies
  MSVC-compatible syntax/ABI, including clang-cl; it is not OS detection.
- `wowdecomp_filter_platform_sources` selects OS overlays and CPU leaves.
  `Posix/` is shared native code; `SDL/` is the current native window/input layer;
  `Linux/` is a Linux provider; `Darwin/` is reserved for the receiving port.
  `Posix/Arch/X86/` owns native CPUID and floating-point instructions; future
  `Posix/Arch/ARM64/` implementations must not advertise invented x86 features.
- Windows retains its original Win32/Win64 layout guards. Selected BlizzardCore
  algorithm files are still explicitly reused from `Win64/` to preserve original
  source/verification metadata. Their directory name is not a platform decision.

This is compile-time provider selection behind the existing engine APIs, not a
new runtime platform framework. SDL is not required for every platform merely
because it is used by the native ports.

## Shared paths now available

| Layer | Shared implementation |
|---|---|
| Window, clipboard, text input, joystick, event dispatch | `Engine/Source/Os/SDL/` |
| Dynamic libraries, TLS, clocks, directory/path adaptation, URL bridge, version-file hashing | `Engine/Source/Os/Posix/` |
| CPUID decoding/cache | `Engine/Source/Os/ProcessorFeatures.*`; the query is under `Posix/Arch/X86/` |
| Storm locks, diagnostics, CRT strings, archive adapter and lifecycle | `Engine/Source/Storm/Source/Posix/` |
| BlizzardCore locks, TLS, clocks/calendar leaves, logging lifecycle, curl downloads, processor count/cache | `Engine/Source/BlizzardCore/Source/System/*/Posix/` |
| Native graphics window/display integration | `Engine/Source/Gx/CGxDevice/SDL/` and `CGxDeviceD3d/SDL/` |
| Audio output | `Engine/Source/SoundEngine/fmodex/sdl/src/` |
| Local-only audio networking boundary | `Engine/Source/SoundEngine/fmodex/posix/src/` |
| Client entry point and fail-closed security boundary | `WoW/Source/Posix/` |
| Unsupported PE-module/updater integration | `Battlenet/Unavailable/`, `Glue/Unavailable/` |

These are reusable implementations, not evidence that every API behaves identically
on Darwin. Their contracts still need real-platform testing.

## Remaining integration inventory

| Boundary | Existing Linux implementation / receiving macOS work |
|---|---|
| Executable location | `Os/Linux/LegacyPath.cpp`, Storm `Linux/SLog.cpp`, BlizzardCore `Process/Linux/Process.cpp` use `/proc/self/exe`. Supply Darwin executable-location leaves, retaining bounded/truncation behavior and executable-relative data/log paths. |
| Files and direct I/O | `Os/Linux/OsFile.cpp` uses `renameat2`; BlizzardCore `File/Linux/` uses Linux timestamp fields and `O_DIRECT`; `Packages/iounit/Linux/IOPlatform.cpp` uses direct-I/O alignment. Implement no-replace rename, timestamps and cache/direct-I/O semantics explicitly; don't silently drop requested guarantees. |
| Memory and diagnostics | `Os/Linux/OsMemory.cpp`, Storm `Linux/SMem.cpp`, BlizzardCore `Debug/Linux/Native.h` read `/proc`. Replace mapping/accessibility/debugger queries. |
| Entropy | `Os/Linux/OsSecureRandom.cpp` uses `getrandom`. Supply a Darwin cryptographic entropy source with the same all-or-fatal contract. |
| System and loaded-image queries | `Os/Linux/System.cpp` uses `sysinfo`, Linux reporting and x86 clock queries; `SystemExtras.cpp` uses ELF loader interfaces. Split native system data from CPU queries and Mach-O image enumeration. |
| Threads | `Os/Linux/OsCall.cpp`, Storm `Linux/SThread.cpp`, BlizzardCore `Thread/Linux/Thread.cpp` and FMOD `linux/src/fmod_os_misc.cpp` contain thread IDs, Linux naming/scheduling/affinity. Preserve shared lifecycle/ownership while replacing leaves; Darwin affinity is not a Linux CPU mask. Don't fake successful priority/affinity changes. |
| Registry/preferences | Storm `Linux/SReg.cpp` implements secure, user-only XDG storage. Choose Darwin storage location while retaining permissions, locking and no machine-wide fallback. |
| Main-thread startup | `WoW/Source/Linux/Startup.cpp` raises the Linux main stack budget; `Posix/Main.cpp` retains SDL ownership on main. Validate Darwin stack provisioning without moving Cocoa/SDL work off main. |
| ARM64 arithmetic/SIMD | Native FP and CPUID have explicit x86 leaves. Shared `Model2/M2Model.cpp`, `Model2/M2Scene.cpp`, `ComponentCore/CharacterComponent.cpp`, and ComSat SPIRIT still contain x86-oriented paths. Audit selected code and vendor CPU flags; provide tested scalar/ARM alternatives. |
| Socket semantics | `WowServices/PosixSocket.h` uses `MSG_NOSIGNAL`. Supply Darwin socket-level SIGPIPE suppression and validate error/nonblocking behavior. |
| Audio platform support | Shared SDL output is separated; FMOD OS services still use Linux IDs and unnamed semaphores. Darwin must replace these, audit legacy `PLATFORM_MAC_INTEL` assumptions and choose the same software-mixer capabilities as SDL Linux. |
| Build/link and libraries | `WoW/Source/CMakeLists.txt` still uses GNU archive rescan groups for Linux cycles. Resolve the native static-library graph with Apple's linker; audit `.so` assumptions, `.dylib` runtime search paths, SDK/deployment target, SDL discovery, movie decoder and shader compiler deployment. |
| Tests | `Tools/Portability` remains deliberately Linux-only. GNU `--wrap`, ELF symbol names, `/proc`, syscall fixtures and `bwrap` isolation are not portable. Split common contract tests from OS fixtures; retain isolation without pretending skipped hardware tests passed. |

### Graphics paths

Preserve both requested macOS routes:

1. Existing D3D9 backend -> **DXVK Native** -> Vulkan loader -> MoltenVK -> Metal.
2. Native Vulkan backend with existing extracted Slang shaders -> Vulkan loader
   -> MoltenVK -> Metal.

DXVK Native must itself have a qualified macOS/architecture-compatible build,
SDL WSI, and feature requirements supported by the selected MoltenVK version.
Portability handling in our native Vulkan renderer does not change DXVK internals.
The Vulkan backend already requests portability enumeration/subset extensions.
Real feature/format limits, presentation, shader bindings, Retina drawable sizes,
resize/minimize, readback and rendering parity still need validation on the Mac.

A future direct Metal backend can reuse the extracted Slang shaders; it is not
part of this platform prefactor and must not require re-extracting the corpus.

## Validation and receiving-machine sequence

Host-independent identity/source-selection tests (no game data or SDL):

```sh
cmake -S Tools/Platform -B out/platform-tests -G Ninja
cmake --build out/platform-tests
ctest --test-dir out/platform-tests --output-on-failure
```

On macOS, run the same commands separately with
`-DCMAKE_OSX_ARCHITECTURES=arm64` and `-DCMAKE_OSX_ARCHITECTURES=x86_64` as the SDK
and execution environment permit. CMake script cases simulate selection only;
they are not cross-compilation, linking or runtime evidence.

Linux regression checks:

```sh
cmake -S Tools/Portability -B out/portability-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWOWDECOMP_MSVC_BASE=backup/linux-native-before-platform
cmake --build out/portability-tests --parallel 4
ctest --test-dir out/portability-tests --output-on-failure
cmake -S . -B out/linux-client -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDXVK_NATIVE_ROOT=/path/to/existing/dxvk-native/prefix
cmake --build out/linux-client --target Wow --parallel 4
```

Use GCC and Clang for the portability suite. The MSVC-macro check does not prove
an MSVC build or binary matching. See the Vulkan branch README for its existing
pinned local shader dependencies, build and shader-corpus tests.

On the receiving machine: first prove platform identity, then implement Darwin
leaves and portable test fixtures, then enable the Darwin client gate. Prove
x86-64 and ARM64 independently. Qualify Vulkan/MoltenVK and DXVK/MoltenVK
separately with startup, local archive reads, login/EULA, input, clipboard, audio,
fullscreen/Retina resize, screenshots and clean shutdown. Do not disable data
signature checks or relax fail-closed unsupported services just to reach a frame.
