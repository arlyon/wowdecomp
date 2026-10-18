# Platform bring-up and macOS handoff

Start with [Platform boundaries](PlatformBoundaries.md). That document is the
source-level integration map: contracts, native algorithms, source selection,
POSIX services, Linux/Darwin leaves, and x86 versus ARM64.

## Stack

`main -> base -> posix -> linux -> vulkan`

- Base adds integration surfaces and native algorithm translation units while
  retaining the original Windows implementations. It is not a de-Windows rewrite.
- POSIX provides the reusable native services and optional SDL adapters.
- Linux supplies kernel/process/filesystem/scheduling/entropy leaves and the
  executable integration. Darwin should start from POSIX, not implement Linux APIs.
- Vulkan adds the renderer and independently selectable DXVK/Vulkan builds.

The old branch tips and original port history are retained locally in backup refs.
The separate original `native-vulkan` worktree has concurrent work which is not
included in this stack.

Darwin and ARM64 client configurations deliberately remain gated. Platform tests
recognize those target identities without claiming their implementations exist.
Build macOS architectures individually; universal binaries are not supported yet.

## Receiving-machine sequence

1. Run the independent base tests with the actual target compiler/SDK:

   ```sh
   cmake -S Tools/Platform -B out/platform-tests -G Ninja
   cmake --build out/platform-tests --parallel 4
   ctest --test-dir out/platform-tests --output-on-failure
   ```

   On macOS, select `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64`. The simulated
   CMake cases check selection only; they do not replace real cross compilation.

2. Run the shared provider checks on the POSIX branch:

   ```sh
   cmake -S Tools/Posix -B out/posix-tests -G Ninja
   cmake --build out/posix-tests --parallel 4
   ctest --test-dir out/posix-tests --output-on-failure
   ```

   These exercise real clocks, recursive locking, TLS and shared-library loading.
   The pthread lifecycle is compiled against declared OS leaves, not linked to a
   fake scheduling provider. Full lifecycle runtime tests belong to OS integration.

3. Implement the Darwin leaves listed in `PlatformBoundaries.md`. Preserve error,
   ownership, blocking, shutdown and main-thread contracts. Qualify FMOD platform
   configuration, stack provisioning, Mach-O/image lookup and Apple linker/runtime
   paths; these are not supplied by the POSIX label.

4. Supply ARM64 CPU/FP/SIMD implementations where required. A 64-bit layout does
   not imply CPUID or SSE. Do not advertise x86 instructions as a compatibility hack.

5. Enable the Darwin client gate only after its providers compile/link. Validate
   startup, local MPQ reads, login/EULA, input/IME/clipboard, audio, drawable sizing,
   fullscreen/resize, screenshots and clean shutdown on the real machine.

## Graphics

Keep both requested routes independently qualified:

- D3D9 -> DXVK Native -> Vulkan loader -> MoltenVK -> Metal.
- Native Vulkan with existing extracted Slang shaders -> Vulkan loader ->
  MoltenVK -> Metal.

`WOWDECOMP_DXVK` and `WOWDECOMP_VULKAN` are independent options on the Vulkan
branch. Vulkan-only does not require DXVK; neither enabled is a configure error.
`WOWDECOMP_GX_BACKEND=dxvk|vulkan` selects only a built backend. The Vulkan backend
requests portability enumeration/subset extensions, but this does not qualify
DXVK's own WSI, architecture or device-feature requirements on MoltenVK.

A WebGPU or direct Metal backend is separate work. Neither requires re-extracting
our existing Slang shaders, and neither is implemented by this boundary change.

## Linux validation

```sh
cmake -S Tools/Portability -B out/portability-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWOWDECOMP_MSVC_BASE=main
cmake --build out/portability-tests --parallel 4
ctest --test-dir out/portability-tests --output-on-failure
```

Use GCC and Clang. Keep the existing three client build configurations on the
Vulkan layer: DXVK-only, Vulkan-only, both. See its README for pinned local shader
dependencies and `Tools/Vulkan` checks. `Tools/Portability` remains Linux-specific:
GNU linker wrapping, syscall fixtures and bwrap device isolation need real Darwin
alternatives before those tests can be described as portable.

Windows source preservation and MSVC-macro preprocessing are not proof of an MSVC
build or binary matching. No macOS compile/runtime, ARM64 execution, MoltenVK
qualification or rendering parity claim follows from Linux checks. Do not relax
archive signatures or fail-closed unsupported/security boundaries to get a frame.
