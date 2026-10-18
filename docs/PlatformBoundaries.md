# Adding a platform: integration map

This is an additive extension surface, not a Windows-to-POSIX rewrite. Existing
Windows APIs and implementations remain the default Windows path. The stack is
`main -> base -> posix -> linux -> vulkan`; a future Darwin provider branches from
`posix`, not from the Linux implementation.

## Ownership

- **base** owns target identity, declarations, provider-selection rules, and the
  native-extension algorithm translation units below. It does not implement a
  Linux kernel service, require SDL on Windows, or claim Darwin works.
- **posix** implements reusable pthread, clock, loader, socket, and SDL services.
  SDL is an optional shared native adapter, not a synonym for POSIX.
- **linux** implements the Linux leaves and wires the Linux executable/tests.
- **vulkan** adds the renderer, shader compilation, and backend selection.

## Source selection, not conditional algorithms

`wowdecomp_filter_platform_sources` selects the OS and CPU overlays. `Native/`
contains additive native-port integration code; Windows excludes it and keeps its
original translation units. Native code never imports `Win64/*.cpp`. The source
checks in `Tools/Platform` reject OS conditionals in the new Native algorithms,
Linux syscalls in the reusable POSIX providers, and changes to 19 Windows
translation units that have explicit native/POSIX counterparts.

The native extension keeps the already-validated native algorithms in separate
translation units rather than modifying original Windows function bodies. This
preserves the Windows reconstruction path; it is not a new runtime abstraction
framework. A small amount of conditional type selection remains in public headers
where Windows and native handles/layouts differ.

## Where to attach Darwin

Paths below are relative to `Engine/Source/BlizzardCore/Source/` unless qualified.

| Area | Base integration / contract | Shared provider | Darwin work |
|---|---|---|---|
| Assertions | `System/Debug/Native/Reporting.cpp`, `Debug/Native.h` | Existing assertion formatting | Implement `System_Debug::Native` debugger detection, directory creation and append opening under `Debug/Darwin/`. |
| Time | `System/Time/Native/Time.cpp`, `Time/Native.h` | `Time/Posix/Native.cpp` | Qualify POSIX clocks/calendar behavior for the deployment target; replace only differing leaves. |
| Logging | `System/Log/Native/{Log,Headers}.cpp`, `Log/Native.h` | `Log/Posix/Log.cpp` | Supply executable-directory/process leaves and qualify native path behavior; logging algorithms need no Darwin conditionals. |
| CPU | `System/Hardware/Native/Features.cpp`, `Hardware.h` | `Hardware/Posix/Hardware.cpp` supplies processor count/cache | Implement the architecture query separately from OS processor information; never invent x86 CPUID/SSE support for ARM64. |
| Worker pools | `System/Thread/Native/{Pools,Worker}.cpp`, `Thread.h` | `Thread/Posix/Thread.cpp` owns publication, registry, references, join and shutdown | Implement `Thread/Posix/Platform.h` under `Thread/Darwin/`: opaque scheduling state, IDs, names, priority and affinity. Unsupported operations must report that honestly. |
| Files | `System/File/File.h`, `File/Native/Throttle.cpp` | Existing file/archive algorithms | Implement the existing File/Filestack API under `File/Darwin/`; executable paths, metadata, no-replace rename and direct-I/O behavior are real OS differences. |
| Unbuffered archive I/O | `Packages/iounit/Native/{IOFileUnit,Unbuffered}.cpp`, `iounit/IOPlatform.h` | Aligned range planning and copying | Implement alignment and positioned unbuffered reads under `iounit/Darwin/`; do not silently drop requested guarantees. |
| Streaming | `Packages/Streaming/Native/{Manifests,Storage,Streaming}.cpp`, existing streaming headers | Queue/storage algorithms | Keep the current explicit local-only boundary until a real streaming provider is implemented. |
| Network | `WoW/.../WowServices/WowConnection*.h` | `WowServices/Posix/` and `PosixSocket.h` | Implement `WowSocketSend` in `WowServices/Darwin/Socket.cpp`, including Darwin SIGPIPE policy and EINTR/nonblocking behavior. |
| D3D core | `Engine/Source/Gx/CGxDeviceD3d/Native/Device.cpp` | Gx SDL platform adapter | Native D3D algorithms contain no per-operation Windows guards; window/library/cursor operations come from the adapter. Windows retains its original D3D source. |
| Windows/input | `Engine/Source/Os/Window.h`, current Gx platform member boundaries | `Os/SDL/`, Gx SDL adapters | Preserve main-thread ownership, qualify drawable/logical sizing, surface creation, focus and text input. Graphics API surface flags belong to the renderer adapter. |
| Process/entropy/memory | Existing Os and BlizzardCore public APIs | Shared callers | Implement Darwin executable/image lookup, secure entropy and memory-map queries in the corresponding Darwin providers; no `/proc` fallback. |
| Audio | Existing FMOD OS callback interface | `fmodex/sdl/` output | Supply Darwin FMOD OS services, especially thread IDs and semaphore behavior. Linux's unnamed semaphore implementation is not the shared provider. |
| Startup | `WoW/Source/Native/Startup.h` | `WoW/Source/Posix/Main.cpp` | Supply main-thread stack provisioning under `WoW/Source/Darwin/`; do not move Cocoa/SDL ownership to a worker. |

## Architecture is not OS or pointer width

- `WOWDECOMP_OS`: `Windows`, `Linux`, `Darwin` (target, not host).
- `WOWDECOMP_CPU`: `x86`, `x86_64`, `arm64`; `Platform.h` exposes corresponding
  zero/one compiler-target macros.
- `WOWDECOMP_POINTER_BITS` and legacy `WOWDECOMP_ARCH_32/64` describe layout only.
- Compiler family and MSVC-compatible ABI are independent decisions.
- Runtime CPU capabilities are queried, not inferred from a 64-bit pointer.

The native x86 CPU leaves live in `Os/Native/Arch/X86/`,
`Hardware/Native/Arch/X86/`, and `WoW/Source/Native/Arch/X86/`. Source selection
excludes them for ARM64 and each leaf asserts the correct target architecture.
ARM64 requires its own leaves and replacements for the remaining x86 SIMD paths
in Model2/CharacterComponent/SPIRIT. The client gate stays explicit until these
exist. Simulated CMake architecture cases do not prove an ARM64 binary works.

## Validation

`Tools/Platform` is independent of Linux providers, SDL, DXVK and curl. It checks
identity, source selection, public boundary declarations and preserved Windows
sources. Run it on base alone using CMake/build/CTest; use GCC and Clang here,
then a real MSVC build and the real Apple SDK on the receiving machines.

`Tools/Posix` checks the reusable provider layer without Linux leaf implementations.
The Linux integration continues to run `Tools/Portability`; the Vulkan layer adds
its existing shader tests and independent DXVK/Vulkan build matrix. No provider
may return fake success merely to make a platform or test appear supported.
