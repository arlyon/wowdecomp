# Local DXVK CPU identity extension (v1)

This directory maintains a **local modification**, not an upstream DXVK API.
Upstream DXVK v3.1.1 resolves to commit
`b1a1c99ab52b687cf950d62c88bc2fa316b41663` (annotated tag object
`1e0f92318d6ec7b7286e050d9e24fd01a2d09c92`). Preserve the upstream license.

The pre-existing installed library reports v3.1.1 and its public d3d9.h is
byte-identical to this pinned source. Its original build provenance is unknown.
This is a **new local build from the observed upstream version**, not a
reconstruction or assertion of the installed binary's exact source provenance.
No other repository, installed dependency, or system prefix is modified.

`d3d9_cpu_identity.h` is the complete CPU-only consumer ABI. It is also included
verbatim by `dxvk-v3.1.1-cpu-identity.patch`; keep these copies identical. The new
IID provides a separate IUnknown vtable: existing interop IIDs/vtables are not
extended. Version 1 requires exact size/version, rejects invalid ordinals, and
queries the physical adapter owned by the SAME IDirect3D9 instance. DXVK 3.1.1's
Vulkan 1.3 requirement guarantees the queried ID/driver properties. Only the
dependency uses Vulkan declarations. Future ABI changes require a new IID.

Both raw deviceUUID and driverUUID are returned, alongside plain uint32 driver,
vendor and device IDs and a bounded name. Saved preferences retain the existing
`deviceUUID:uint32-driverId` spelling; driverUUID is metadata, not an implicit
preference migration. Neither GUID/LUID nor name nor unrelated enumeration is
used to infer identity.

Build into a fresh isolated output with existing reviewed tools/dependencies:

```sh
sh ThirdParty/dxvk-identity/build-local.sh "$PWD/out/deps/dxvk-identity"
cmake -S Tools/Dxvk -B out/dxvk-identity-tests \
  -DDXVKNative_INCLUDE_DIR="$PWD/out/deps/dxvk-identity/source/include/native/directx;$PWD/out/deps/dxvk-identity/source/include/native/windows" \
  -DDXVKNative_D3D9_LIBRARY="$PWD/out/deps/dxvk-identity/build/src/d3d9/libdxvk_d3d9.so"
cmake --build out/dxvk-identity-tests --parallel 2
ctest --test-dir out/dxvk-identity-tests -R '^DxvkAdapterIdentity$' --output-on-failure
```

The script fetches pinned upstream recursive submodules, enables only native
SDL2/D3D9, and builds exclusively below its explicit local output, without an install step.
`provenance.json` records the source/archive, recursive pins, patch/header hashes,
flags and observed library hash for this delivery. It is evidence, not a source
of paths that override the caller's isolated output.

The production adapter leaf uses CPU shared selection policy and paired ordinals
for CreateDevice and all corresponding D3D caps/format probes. Successful
CreateDevice is checked using GetCreationParameters on that same D3D instance.
Old dependencies without the IID retain ordinary ordinal fallback and saved
preferences, report identity unavailable and never fabricate exact selection.
Presentation compatibility is unverified before CreateDevice.

`Tools/Dxvk` includes a deterministic two-adapter COM fixture and an asset-free
real-library WSI fixture. Run the latter only under the shared GPU flock, pinned
ICD and explicit separate core/sync validation environment. It creates fixture
windows/devices, not a game. The old-library mode `--old-extension` verifies real
QI failure without claiming selection support. Single-GPU runs do not establish
multi-GPU acceptance; fakes prove mapping logic, not actual GPU selection.
