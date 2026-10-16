# macOS bring-up: draft checkpoint

Stack: `main -> base (#2) -> posix (#5) -> macos`, parallel to `linux` (#3).
This is work in progress, not a working post-restack client release.

## Worth preserving

- Adaptation of coolzoom/zz's macOS port from
  [SatyPardus/wowdecomp#8](https://github.com/SatyPardus/wowdecomp/pull/8), snapshot
  `e4297f04bf25c831d30740ee063bbd6127ec37e2`: SDL2/OpenGL 2.1 platform code,
  Darwin OS providers, ARM64 SIMD compatibility and runtime bug fixes.
- A standalone [D3D9 GPU smoke test](../Tools/DxvkSmoke/README.md) with pinned
  macOS DXVK Native instructions. It checks triangle rendering, exact GPU
  readback pixels and presentation through MoltenVK/Metal without game data.
- Unsupported security operations fail explicitly, following the Linux
  reference, rather than silently returning clean results.

The upstream launch scripts and `.DS_Store` files were not imported. Imported
[porting notes](../notes/macos-port.md) retain upstream findings for reference;
they do not establish that this adaptation supports gameplay.

## Evidence and its limits

Before restacking onto the new POSIX layer, Apple Clang 21 produced a complete
ARM64 Release client binary with OpenGL and the optional DXVK backend enabled.
That result belongs to the preserved local `backup/macos-before-posix` snapshot,
not this PR's current source selection. No local game-data runtime test occurred.

The standalone D3D9 smoke passed on Apple M4 Max/macOS 26.7 with the pinned
macOS DXVK Native fork and Homebrew MoltenVK. Expected background `204080` and
triangle `ff0000` were read back, and Present succeeded. This does not qualify
all D3D9 features or the WoW renderer. Upstream stock DXVK is not claimed to
support this configuration.

After restacking, `git diff --check` passed and ancestry was verified. Attempts
to run `Tools/Platform` and configure the client were blocked by the machine's
Xcode licence gate (`/usr/bin/c++` exits 69). No post-restack compile/test success
is claimed, including the platform/POSIX suites. Alex must review/accept the
licence before compiler-based validation resumes.

## Required before marking ready

- Reconcile imported `Mac/` providers with the new `Darwin/`, `Native/`, `Posix/`
  and `SDL/` selection contracts. Remove duplicate implementations, implement the
  Darwin thread/socket/debug/CPU leaves, and preserve original Windows sources.
- Wire the imported OpenGL renderer alongside the new native D3D core. The
  pre-restack optional-backend selection was not preserved by shared-file
  conflict resolution; this checkpoint does **not** claim a selectable dual
  renderer client. The standalone DXVK smoke remains independent.
- Review inherited CPU capability reporting and successful no-op services;
  ARM64 must not claim invented x86 hardware features.
- Run the platform and POSIX suites, full client builds and graphics smoke;
  qualify OpenGL-only and dual-renderer builds separately.
- With locally supplied 3.3.5a (12340) data, verify startup, rendering, input,
  Retina/resize/fullscreen, audio and shutdown. Keep archive signature checks
  enabled and unsupported security operations explicit.

No game assets or credentials are included. No Windows or Intel macOS build has
been verified in this work.
