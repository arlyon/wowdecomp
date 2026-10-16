# macOS normal-quit smoke

This **interactive desktop integration test** starts the real client and injects
an SDL window-close event after eight seconds. It must exit normally, including
Blizzard::Using's static shutdown callbacks. It caught both the log admission
mutex and thread creation mutex being destroyed before those callbacks on Darwin
(exit 134 instead of 0). It does not test login or gameplay.

Use a separate configured runtime directory with game assets, as described in
`docs/MacOSBringup.md`. The test can update that runtime's configuration/logs just
as an ordinary launch does. Do not run another copy of that client concurrently.

From the repository root, after building the client:

```sh
cmake -S Tools/MacSmoke -B out/mac-smoke \
  -DWOWDECOMP_CLIENT="$PWD/bin/Mac/Wow"
cmake --build out/mac-smoke
SDL_VULKAN_LIBRARY=/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json \
ctest --test-dir out/mac-smoke --output-on-failure
```

Adjust the Homebrew paths for your installation. Validation can also be enabled
with the usual `WOWDECOMP_VULKAN_VALIDATION` and `VK_LAYER_PATH` variables. By
default the client selects its enabled-build backend. Set
`-DWOWDECOMP_SMOKE_BACKEND=dxvk` or `vulkan` at configure time for an explicit
selection; that backend must be built. Do not use this test for a visual verdict
on rendering.
