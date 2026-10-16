# Native D3D9 graphics smoke

This exercises a real SDL2 window, D3D9 HAL device, fixed-function triangle,
GPU readback with exact pixel assertions, and presentation. No game data is
needed. It does not prove that the WoW client builds or runs.

## macOS arm64

Use mise for CMake, Ninja and Meson. Install native libraries with Homebrew:

```sh
brew install sdl2 molten-vk vulkan-loader glslang
```

The Linux DXVK dependency remains unchanged. For this macOS test, use the
macOS-capable DXVK Native fork, pinned rather than following its branch:

```sh
git clone https://github.com/fbraz3/dxvk.git out/deps/dxvk-macos
git -C out/deps/dxvk-macos checkout 8703715
git -C out/deps/dxvk-macos submodule update --init --recursive
meson setup out/deps/dxvk-mac-build out/deps/dxvk-macos \
  --buildtype=release --prefix="$PWD/out/deps/dxvk-mac-prefix" \
  -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
  -Denable_d3d8=false -Ddxvk_native_wsi=sdl2
meson compile -C out/deps/dxvk-mac-build -j 8
meson install -C out/deps/dxvk-mac-build

cmake -S Tools/DxvkSmoke -B out/dxvk-mac-smoke -G Ninja \
  -DDXVK_NATIVE_ROOT="$PWD/out/deps/dxvk-mac-prefix" \
  -DCMAKE_BUILD_RPATH="$(brew --prefix)/lib"
cmake --build out/dxvk-mac-smoke
SDL_VULKAN_LIBRARY="$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib" \
VK_DRIVER_FILES="$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json" \
  ctest --test-dir out/dxvk-mac-smoke --output-on-failure
```

The rpath is needed for DXVK's dynamic SDL2/Vulkan library discovery. The SDL
Vulkan library override selects the same loader used by DXVK. No library shims
or system-wide environment changes are required.

Verified on Apple M4 Max, macOS 26.7, Apple Clang 21, Homebrew SDL2 compatibility
2.32.74/SDL3 3.4.18, MoltenVK package 1.4.2 and Vulkan loader 1.4.363.0.
The test produced background `204080`, triangle `ff0000`, and successful Present.
Stock upstream DXVK was not qualified for macOS; the macOS fork's feature
fallbacks must not be confused with upstream support or full game compatibility.
