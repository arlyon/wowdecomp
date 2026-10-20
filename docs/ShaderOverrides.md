# Maintained native Vulkan shader overrides

The native Linux backend selects readable local source **before** compatibility
recovery. Both origins use the same Slang compilation, Gx bindings, uploads and
Vulkan pipeline ownership. Maintained source does not invoke MojoShader; only an
absent override permits bytecode recovery. An override is not evidence of RT
participation or complete material coverage.

## Identity and location

The default root is the configured CMake path `WOWDECOMP_VULKAN_OVERRIDE_DIR`
(default: this source tree's `Shaders/Overrides`). Set the process environment
`WOWDECOMP_SHADER_ROOT` to choose another local root.

```text
<root>/vertex/vs_3_0/Terrain/17.slang
<root>/pixel/ps_3_0/Terrain/17.slang
<root>/shared/Lighting.slang
```

Names preserve the original Gx key's spelling. `Terrain:17` becomes name
`Terrain`, permutation `17`; a key without a suffix means permutation zero.
The profile is taken from the actual loaded SM1–3 version token, including any
original archive-profile fallback. Stage, profile and permutation are separate
path components, not material hashes or permission lists. Anonymous synthetic
programs without a Gx key retain explicitly anonymous compatibility rendering.

Entry and include files share a pinned-directory reader. Every descendant
component is opened without following symlinks; escaping paths, descendant
symlinks (including dangling links), directories, FIFOs, empty/NUL-containing
files and unreadable files are errors. Files are limited to 4 MiB each and a
source generation to 32 MiB total. No network loading or archive modification is
performed. Root-relative includes such as `#include "shared/Lighting.slang"`
work through the same confined snapshot.

## Binding interface v1

Compiled Slang reflection, not recovered arithmetic, validates maintained source.
The backend consumes one common binding/packing layout for both origins:

| Stage | Descriptor interface |
|---|---|
| Vertex | Combined ordinary texture samplers at set 0, bindings 0–15 |
| Vertex | Optional full float register file at set 1, binding 0: one `float4[256]` field, 4096 bytes |
| Vertex motion | Previous full register file at set 1/binding 1; one float4 jitter block at binding 2 |
| Pixel | Combined ordinary texture samplers at set 2, bindings 0–15 |
| Pixel | Optional full float register file at set 3/binding 0 |
| Pixel | Required one-float4 alpha-test block at set 3/binding 1; threshold is `.x` |
| Pixel motion | One float4 motion-options block at set 3/binding 2 |

Vertex inputs are float vectors at the existing Gx locations 0–13; position
location 0 and a float4 `SV_Position` output are required. Pixel output requires
float4 C0; motion/Classic-companion slots C1/C2 retain their existing meanings.
Integer/bool application registers, arbitrary storage resources, separate texture
and sampler bindings, arrays of resource descriptors and new descriptor sets are
not part of v1. A demonstrated lighting requirement must extend this interface
explicitly rather than smuggling resources through an unchecked binding.

Compatibility source keeps its original sparse register packing and immutable
DEF precedence. Maintained source explicitly exposes the full register file when
needed; both are filled by the same layout-driven upload implementation.

## Variants, failures and source freshness

AA, fog-interface and float-output variants derive from the **selected source**,
never from another recovery of the original bytecode. The retained AA/HDR
adapters currently use the existing structural source ABI (`main_Input`,
`main_Output`, `m_oC0`, `return output;`, and the usual generated-style entry
signature). Keep the register block directly in the entry file for the retained
AA adapter, and pass register/pose values explicitly to shared helper functions
rather than having helpers implicitly read current-pose globals. Missing
previous-register/motion bindings fail the AA variant instead of certifying a
second current-pose evaluation. Use the scaffold for overrides requiring those
optional modes. These are interface adaptations, not reconstruction of material lighting.

Present malformed or interface-incompatible source fails shader creation and
reports the identity, source origin and compiler/interface diagnostic. It never
silently substitutes generated source. Existing effect-level raster rollback
still uses the selected shader; a fallback frame is not effect completion.

Loaded entry/includes are immutable for that shader generation. Hot reload is
not automatic: explicit shader recreation/reload obtains a new snapshot and a
fresh process-local pipeline identity. Compiler profile, row-major options and
interface v1 are fixed for that compiler instance; temporal/fog/output options
have generation-owned variant entries. No application-owned persisted shader
cache exists (the GPU driver may maintain its own ordinary cache).
Identical base SPIR-V from different source generations cannot alias an HDR
source entry. Editing a source file and explicitly reloading cannot reuse the
old derived pipelines.

## Offline inventory groundwork (not the RT coverage gate)

Build `RendererAssetInventory` in `out/portability-tests`. Its arguments are a
**new report directory**, followed by the exact declared read-only archives in
lowest-to-highest priority order. The report must be outside the asset directories;
existing nonempty reports are never overwritten. For example:

```sh
out/portability-tests/RendererAssetInventory out/corpus-run /external/Data/common.MPQ ...
```

The example's ellipsis must be replaced by the full declared archive list, not a
sample or an arbitrary filesystem glob. Archive order and sizes appear in
`archives.tsv`. The tool parses complete listfile lines (including a final line
without a newline), rejects unsafe/overlong names, and reconciles the union of
names against **every active MPQ hash row**, including locale/platform entries.
Unknown rows fail rather than disappearing from the denominator. Compression of
archive tables is supported; only their optional in-memory compaction is disabled
so the original hash rows remain available for inspection. Archive write mode and
direct disk asset fallback remain disabled.

`assets.tsv` records final resident names, authored tombstones and stale list
names. `shader-records.tsv` preserves each container/permutation identity even
when bytecode is identical. Text/non-native shader records remain in the census;
the metadata parser does not pretend to validate their shader semantics. Empty
records are explicit. Container failures appear in `shader-errors.tsv`; the tool
continues discovery but returns failure and reports incomplete shader enumeration.

This is **inventory only**: `coverage=UNVERIFIED` is unconditional. It does not yet
parse material uses, select native shader fallback profiles, resolve original
case-sensitive Gx keys, compile required maintained interfaces, or attest a
revision with full archive fingerprints. A reconciled filename table is not
material coverage or RT readiness. The eventual acceptance gate must perform
those additional checks and cannot substitute this inventory's exit status.
Generated shaders/assets remain local; no proprietary payload is exported by this
tool. Synthetic archive tests exercise unknown names, truncation, duplicate
programs, read-only bytes and report confinement without external assets.

## Verification

`ShaderOverride` checks identity, confinement, real compiler includes, immutable
snapshots, fresh generations, malformed/interface cases and bounded inputs.
`ShaderOverrideLiveGpu` runs 48 real selected-source frames across AA, float
transport, independent scene scaling and native UI, then verifies explicit
reload and absent-only generation. Its maintained vertex record deliberately
contains unrecoverable bytecode, making accidental recovery observable.

The fixture sources are handwritten and asset-free. Generated game scaffolding
and proprietary corpus exports remain local. Passing these loader tests does
not establish the subsequent RT shadow, corpus or production-scene contract.
