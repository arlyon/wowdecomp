# Native Vulkan performance investigation — hand-off

## Performance worktree continuation

The user approved the staged CPU optimisation pass and requested a new worktree
on top of `vulkan`. Active destination: `.wt/vulkan-performance`, branch
`vulkan-performance`, initially based on `fork/vulkan` at `2c7ba8b`.
The historical status and capture provenance below remain unchanged as evidence.
The performance investigation is now resumed, targeting 2.5–5 ms/frame
(200–400 FPS), not yet demonstrated.

Profiling tools, FrameProfile analysis/tests, the complete capture directory and
its matching executable, and the pinned FlameGraph source were copied from
`.wt/native-vulkan` and SHA-256 verified. Import inventory:
`out/modern-renderer/profiling-import.json`. Historical absolute paths and PID
references below describe the old capture, not this worktree or a live process.
No build trees, live PID files, game data, account configuration, or credentials
were imported. Configure fresh build/run directories before using capture helpers.

The UI/buffer/residency/cloud commits landed and were fast-forwarded into this
worktree through `991fd53`. Existing worktrees were not modified. Performance
changes are on top of that revision. The scan/staging pass was committed as
`0c6291a`; the texture fast path is a separate follow-up commit. The user explicitly
authorized unsigned commits after a GPG timeout. Signing was disabled only for
those commit invocations; repository/global configuration was not changed.
Nothing was pushed.

### Implemented first pass

- `BufLock` skips the resident-geometry scan for stream pools, which `Geometry`
  never caches. Non-stream overlap invalidation, pool resize/destruction and
  same-frame snapshot replacement remain in place.
- Small resident uploads borrow ranges in a lazily allocated, bounded 4 MiB
  coherent staging arena. Reset happens only after the previous submission fence,
  never on readback/resume. Arena overflow and large uploads retain dedicated
  staging allocations. The existing transient vertex/uniform arena is unchanged.
- Ported opt-in CSV/GPU timestamp profiling from the old worktree, with additional
  lock scan/visit/dirty counts and resident/texture allocation counters. Enable with
  `WOWDECOMP_VULKAN_PROFILE=<new-csv-path>`. No account data is captured.
- Added GPU regression cases for borrowed staging, overflow, padding, invalid
  ranges, same-frame mutation, resize, retirement and the existing 68 MiB mesh.
  `FrameProfile.py` handles both old and new numeric CSV schemas.

### Validation and narrow benchmark evidence

Hardware queried locally: Ryzen AI 9 HX 370 / Radeon 890M, RADV Mesa 26.1.8.
Both Release clients built successfully. Commands from this worktree:

```sh
cmake --build out/client-vulkan --target Wow --parallel 4
cmake --build out/client-vulkan-reference --target Wow --parallel 4
cmake --build out/portability-tests --parallel 4
ctest --test-dir out/portability-tests --output-on-failure
cmake --build out/vulkan-tests --parallel 4
ctest --test-dir out/vulkan-tests --output-on-failure
VK_LOADER_LAYERS_DISABLE='~implicit~' out/vulkan-tests/FixedFunctionTest --upload-benchmark
VK_LOADER_LAYERS_DISABLE='~implicit~' out/vulkan-tests/FixedFunctionTest --upload-benchmark-reverse
git diff --check
```

Results: 44 portability and four Vulkan CTest registrations passed (the latter
includes 10 Python profiling tests). Direct compile-database syntax checks passed
for all four changed C++ translation units. Clangd's default exhaustive tweak
self-tests reported internal refactoring failures; rerunning with `--tweaks=none`
completed with zero errors, consistent with the compiler syntax checks.

The residency fixture additionally passed with the existing reviewed local
Khronos layer and `VK_LAYER_VALIDATE_SYNC=1`; no validation messages were emitted.
See `out/modern-renderer/residency-validation.log` for the result and
`out/modern-renderer/*-build.log` for build output. Existing compiler warnings
remain; they were not suppressed.

Synthetic benchmark: 64 resident 4 KiB buffers mutated in each of 32 test frames
(2,048 uploads), measuring only CPU upload recording, not submission/wait or
whole-game frames. Three repeated comparisons, including reversed execution
order, gave dedicated-staging median times of 0.749–1.882 ms versus reused-staging
0.033–0.125 ms per 64-upload batch. Per-upload staging allocations fell from
2,048 to zero; both paths created 64 resident destinations. The borrowed arena
itself is one allocation, outside this timed loop. This demonstrates reduced
allocation/recording cost, NOT a gameplay FPS improvement or a new baseline.
Raw results: `out/modern-renderer/upload-benchmark-repeated.txt`.

### Matched interactive comparison remains open

A reference source snapshot under `out/performance-reference` is based on
`991fd53`, with identical telemetry/upload API but the original all-pool scan
and dedicated staging behavior. Snapshot provenance:
`out/modern-renderer/reference-source.json`. Build paths:

- Optimized: `out/client-vulkan/bin/Wow`, SHA-256
  `d3d78c884f1facd77a5c41db5e7f9cba7464b9fffc97c022a07bccea201bda33`.
- Reference: `out/client-vulkan-reference/bin/Wow`, SHA-256
  `28bf4a591ed3e3eda67209afc08c8405f288991ac41236c44319ba32dcf3d36d`.

These hashes describe the validated binaries at this handoff; recompute after
rebuilding. The isolated launcher is `out/modern-renderer/run-client.py`:

```sh
python out/modern-renderer/run-client.py <fresh-optimized-label>
python out/modern-renderer/run-client.py <fresh-reference-label> --reference
```

Run ONE client at a time. The launcher preserves old labels, records executable
hashes, refuses interactive validation, and uses `out/runs/performance` for both
variants. It links only the existing WOTLK Data directory; no account configuration
is copied. Login and licence acceptance must be performed by the user. Restore
identical settings/view for each capture and explicitly identify continuous
world-active/focused intervals; CSV draw counts do not prove world continuity.
The initial optimized launch (`performance-optimized-01`) produced 614 numeric
CSV rows and exited with code 0. Its scene continuity was not confirmed, so do
not treat it as an in-world performance baseline or diagnose the exit from its
code alone. Live CSV parsing confirmed that the new counter columns are present.

### Sequential flight observations (not an isolated causal benchmark)

Two 60-second user-operated flights at 1906×1236 are available:

| Metric | Reference | Optimized |
|---|---:|---:|
| Observed FPS from mean interval | 50.36 | 84.92 |
| Interval median / p95 | 18.57 / 33.93 ms | 9.77 / 22.97 ms |
| Record wall-time median | 12.67 ms | 4.47 ms |
| GPU median | 5.08 ms | 4.37 ms |
| Foreground observations | 99.01% | 95.19% |
| Mean geometry entries visited/frame | 156,632 | 1,438 |
| Mean dedicated resident staging allocations/frame | 1.90 | 0 |
| Mean draws/frame | 285 | 328 |
| Mean triangles/frame | 67,227 | 89,047 |

Optimized capture: `out/modern-renderer/flight-focused-20261016-160906`.
The user confirmed continuous in-world flying around Elwynn and Stormwind.
Reference: `out/modern-renderer/reference-flight-focused-20261016-161207`;
the user subsequently confirmed continuous in-world flight in similar, not
identical, conditions.
Comparison JSON: `out/modern-renderer/flight-comparison.json`.

Observed average FPS was about 69% higher with the changes, and mean scan visits
were 99.08% lower. However, these are sequential flights, not identical paths,
views, draws, timings or verified identical settings; focus also differs. Do not
claim a controlled 69% renderer speedup or attribute the whole change to one fix.
The optimized sample contained more geometry/draws on average. GPU and CPU record
wall-times overlap; do not add them or interpret record time as on-CPU time.

The target of 200–400 FPS was not reached in the optimized flight. A repeatable
stationary scene and new profile of remaining CPU work/fence waits are the next
measurement seams; API/frames-in-flight redesign is not implicitly approved.
Non-stream scan cost and texture allocation remain possible targets.

### Fresh optimized CPU capture

`out/modern-renderer/cpu-flamegraph-20261016-162345`: 60.185 seconds,
12,258 samples, 100% observed foreground focus, zero lost samples, 2.54% of
cycle weight with unknown frames, no automated quality issues. User confirmed
world start; ask about loading/logout before asserting continuous world workload.
Frame-pointer build hash:
`86d935cb651aa39ec6195b224a054f9fdf565e65135c88ed4394334176917204`.
All 1,392 C/C++ translation units were audited for frame-pointer flags; normal
Release binary hash remained unchanged. This profiling run observed 72.32 FPS,
12.21 ms median interval and 5.23 ms median GPU time; do not compare this directly
to the earlier normal-build flight as an optimization regression.

Exclusive CPU cycle-weight hotspots (all threads, not wall-frame percentages):

- `inflate_fast` 4.83%; other decompression functions also appear. Attribute to
  streaming/loading versus steady state before selecting an optimization.
- `amdgpu_vamgr_find_va` 4.73%: about 2.95 percentage points under texture
  `MakeImage`, 1.78 under resident destination `Resolve/CreateBuffer`.
  The staging arena does not eliminate image/destination allocation.
- `operator()` 4.38% is NOT one function: 2.21 percentage points are texture-map
  comparisons, 0.75 geometry-map comparisons, 0.51 pipeline-key comparisons.
- `memmove` 2.45%, including roughly 0.97 percentage points on streamed geometry.
- Tree iteration 2.42%, of which about 1.76 percentage points are in ScenePresent,
  consistent with the opt-in profiler summing resident sizes once per frame.
  Do not call this the removed per-stream-lock scan or treat instrumentation
  overhead as production work.
- Descriptor allocation 1.33%, pipeline binding 0.66% exclusive.

### Approved texture lookup fast path

The user approved implementing the resident-texture lookup fast path as another
commit. `State::Texture` now uses `CGxTex::m_apiSpecificData` for existing images
unless recreation is requested. Creation publishes the stable map-entry address;
recreation clears it before erasing the old entry, including the failure path.
Sampler and texel updates still use the common path. Texture/device teardown
already clears the pointer. New opt-in CSV counters are `texture_cache_hits` and
`texture_cache_lookups`; both remain optional in the Python parser.

Validation: direct renderer syntax check, Release client build, all 44 portability
and four Vulkan CTest registrations passed; the Python test count is now 11.
Logs: `out/modern-renderer/texture-cache-*-build.log` and
`texture-cache-syntax.log`. Existing GPU fixtures are regression checks, not
full production texture-recreation failure injection. Live hit-rate/lifetime
smoke checks and matched-flight FPS measurement remain open; the old profiling
client was still running at the end of implementation and was not interrupted.

The initial checkpoint attempt failed because GPG signing timed out. The user
then explicitly requested no signing. The earlier validated performance pass
was committed separately as `0c6291a`, preserving this texture fast-path delta
for its own commit. Use per-command signing overrides, not persistent config
changes. Neither commit authorizes a push.

Remaining candidates are allocation attribution/reuse for images and destination
snapshots, and draw submission/descriptors. Preserve snapshot/lifetime correctness;
no blanket map replacement or memory allocator dependency is justified by these
percentages.

### Approved bounded resource reuse — image step

The user confirmed other CPU builds were running during earlier flights. Those
FPS comparisons are not a controlled baseline; rebuild the baseline from scratch
on a quiet machine after the approved small optimizations.

Implemented texture-image allocation reuse in `ResourceCache.{h,cpp}`. Only a
signalled submission fence can admit retired images. Matching includes extent,
mips, layers, format, aspect and usage; the factory always uses the same optimal
2D/single-sample/exclusive creation policy. Reuse discards old contents through
the normal UNDEFINED-layout initialization path. Old samplers are destroyed after
completion rather than cached. Entry/view addresses in the live texture map
remain stable; the cache owns idle image handles, not live texture bindings.

The default idle cache budget is 32 MiB of actual Vulkan allocation requirements,
with a 128-entry metadata cap. Overflow evicts idle resources; oversized resources
are destroyed normally. Live/in-flight allocations are not part of this budget.
The buffer step below extends the same cache and shares this budget.

Validation passed: Release client build, 44 portability tests and five Vulkan
CTest registrations (12 Python profiling tests). New `ResourceCacheGpu` tests
exact compatibility, unsignalled-fence exclusion, actual GPU texel replacement,
zero/oversized budgets, FIFO image eviction and entry-count limits. It also passed
with the existing local synchronization-validation layer and no messages.
Repeated-image fixture: 32 allocations without caching versus one allocation and
31 reuses with caching, with GPU pixel checks after each update. No FPS claim.
Logs: `out/modern-renderer/image-cache-*.log`.

CSV adds optional `image_cache_reuses`, `resource_cache_bytes` and
`resource_cache_evictions`. Gameplay traversal and a clean-machine performance
baseline remain open. No server, graphics-quality or external dependency changes.

### Bounded resource reuse — resident-buffer step

Texture reuse was committed separately as `6f0ce1b`. The follow-up adds exact-size
resident device-local buffer reuse to the same typed FIFO cache. Images and
buffers together share the 32 MiB requested-allocation budget and 128-entry cap;
it is not a total VRAM bound. Live/in-flight resources remain outside the cache.
Staging/readback buffers and different usage/memory-property allocations are
explicitly excluded. Resources are admitted only after their covering submission
fence completes; a stale fence from an earlier submission is not a valid token.

`ResidentBuffer::Resolve` can acquire completed destinations, but never returns
its current-frame snapshots to the cache. Failure before recording keeps the
existing cleanup path. Resize compatibility remains exact, including padded
buffer capacity, and reused destinations are always freshly uploaded. New CSV
`resident_device_reuses` distinguishes reuse from new destination allocations.

Production GPU fixture coverage now includes both types, shared byte-budget
pressure/eviction in both directions, mapped/usage exclusion, unsignalled-fence
exclusion, recycled same-frame snapshots, resized/padded GPU data, and deterministic
churn counts. With caching disabled/enabled, repeated logical resident-buffer
creation performed 32/1 destination allocations with 0/31 reuses; every GPU
readback matched the expected changing data. The image fixture has the same
32-to-1 allocation result. These are narrow deterministic allocation results,
not a measured gameplay speedup.

Canonical client build, all 44 portability and five Vulkan CTest registrations
passed; optional numeric CSV parsing remains compatible with older captures.
Direct syntax checks passed on the four touched C++ translation units. The full
resource-cache fixture also passed the existing local Khronos synchronization
validation layer with no messages. Logs: `out/modern-renderer/buffer-cache-*.log`.
The older 68 MiB mesh and fixed-function/mixed-stage GPU tests still pass.

Prepare a new profiling build and baseline with other builds stopped by the user;
do not stop unrelated processes automatically. Normal build:
`out/client-vulkan/bin/Wow`; refreshed profiling build/flags/hash audit:
`out/modern-renderer/flamegraph-profile-build.json`. Earlier hashes/captures above
refer to earlier binaries, not this cache implementation. Current implementation
validation evidence/hashes: `out/modern-renderer/resource-reuse-validation.json`.
No server, graphics-quality, allocator-package or frames-in-flight changes.

## Historical status and scope

The user has paused performance investigation in favour of AA and ray-traced lighting/shadows.
No CPU optimisation was implemented from the flame graph. Do not block graphics implementation
on finishing this investigation; do keep measuring feature costs independently.

Worktree: `/home/arlyon/Programming/wowdev/wowdecomp/.wt/native-vulkan`, branch `native-vulkan`,
HEAD `b57a5f3`. Significant renderer/UI/residency work remains uncommitted. Preserve all of it;
do not reset, clean, rebase, stage or commit unrelated changes. The cloud fix was separately
committed and pushed to `fork/posix` as `6dcb614a3d958b891e35528dd67ce7aedf9f0047`.

## Evidence: useful call stacks, NOT a clean whole-session world benchmark

All paths below are relative to this worktree. Capture directory:
`out/modern-renderer/cpu-flamegraph-20261016-145610/`.

- `cpu-flamegraph.svg`: offline zoom/search, CPU-cycle weighted, all threads combined.
- `cpu-by-thread.svg`: same sample set, separated by thread.
- `perf.data`, `perf-record.log`, `perf-stat.csv`, `sample-format.txt`.
- `cpu.folded`, `cpu-by-thread.folded`, `cpu-symbols.json`, `cpu-report.txt`.
- `session.json`, `stack-quality.json`, `frames-summary.json`.
- `client-frames-full-session.csv`: preserved numeric renderer CSV, including frames outside capture.
- `Wow.profile`: preserved matching executable, with symbols/line information.
- `profile-build.json`: original build flags, hashes and translation-unit audit.
- `foreground-interval.json`: proposed longest continuously focused subinterval ONLY;
  a separate filtered graph has NOT been exported. It does not establish world-state continuity.

These ignored artefacts are local, not included in Git. Transfer them explicitly if investigating
elsewhere. The matching executable was copied so later launches/builds cannot silently replace
its symbol evidence. Original recorded executable path is `out/runs/native/Wow`; point any later
symbolisation at the preserved matching binary rather than a new build at that path.

### Capture quality

- 60.099 seconds; 11,100 samples; zero reported lost samples.
- Focused for 97.908% of sampled wall-time observations. Focus loss at elapsed 23.89–25.14 seconds.
- Unknown-frame cycle-weight fraction 1.521%; single-frame fraction 1.130%; depth-limit 0.0199%.
- On-CPU user-space sampling only; not an off-CPU/wait-time or GPU flame graph.
- FP chains can skip/truncate frames in frameless libraries/assembly. Some inline labels are
  unqualified (`operator()`, `DrawBatch`, etc.); do not assume each denotes one C++ method.
- The user subsequently reported being logged out. The precise logout time and cause are unknown.
  **Do not present the entire capture as continuous active-world workload or use its aggregate
  percentages as a representative in-world baseline.** Login/glue/transitions may contaminate it.
- The client later exited with code 0. Its CSV contains 11,401 frames after the capture's last
  included frame (8280), so process exit did not terminate the 60-second recording. This does not
  prove that the network/world session survived the capture. Code 0 alone does not identify why it quit.
- Local world-server PID3297837 had been running for roughly 3h52m when checked; no restart was
  observed. Generic connection-log disconnect tags are NOT sufficient to identify the failure:
  authentication-to-realm handovers can disconnect an auth connection normally.

### Directional results only

Exclusive cycle-weight shares across the whole potentially mixed capture:

| Symbol | Share |
|---|---:|
| `std::_Rb_tree_increment(std::_Rb_tree_node_base*)` | 10.92% |
| `operator()` (ambiguous aggregate label) | 4.18% |
| `amdgpu_vamgr_find_va` | 4.03% |
| `inflate_fast` | 3.86% |
| `__memmove_avx512_unaligned_erms` | 3.26% |
| `_M_lower_bound` | 1.36% |
| `radv_AllocateDescriptorSets` | 1.17% |

For 4919 selected frame rows, interval median/p95/p99 was 8.836/22.687/38.661ms;
record wall-time 3.779/17.080/30.468ms; GPU 2.576/5.244/6.852ms.
These are profiling-build, mixed-focus, potentially mixed-scene observations, NOT an accepted
production-build baseline or a predicted improvement from changing any one hotspot.

## First implementation candidates — not yet approved or applied

1. **Avoid scanning all resident geometry for streaming-buffer locks.**
   `Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.cpp`, `CGxDeviceVulkan::BufLock`
   currently iterates `m_native->geometry` for every lock, including stream pools.
   Resident `Geometry()` excludes stream usage, so examine a non-stream guard first.
   Retain overlap invalidation for non-stream aliases, pool resizing, same-frame snapshots and
   destruction-before-submission. Source inspection proves the scan exists; the current graph
   does NOT prove all 10.92% of tree iteration comes from it. Confirm with call-site evidence/A/B.
2. **Investigate Vulkan allocation churn.** `amdgpu_vamgr_find_va` appears below RADV memory
   allocation. Residency staging/new snapshots are candidates; attribute allocations before
   introducing an allocator or pool. Do not equate resident-byte counters with total VRAM usage.
3. Texture lookup, draw/uniform scratch reuse and redundant pipeline binds were earlier candidates.
   The prior flat 5.7-second/820-sample slice is weak evidence, not a replacement for new A/B data.
4. Attribute archive decompression to loading/traversal before optimising steady-state rendering.

Use the smallest measured change; do not rewrite shaders or lower resolution to treat CPU work.
The user prefers correct persistent meshes to increasing transient-arena limits.

## Reproduction and next investigation

Tools: `out/modern-renderer/{run-client.py,capture-flamegraph.py,fold-cpu-stacks.py,
render-cpu-flames.py,build-flamegraph-profile.py}`. Inspect arguments/current state before running.
The capture helper currently defaults to the **exited** `flamegraph-profile-retry` label.
Never reuse old PID435977 as a live process without checking `/proc` and executable hash.

1. Establish whether logout happens without sampling. Reproduce with user-authenticated world
   sessions; do not automate login or licence acceptance. Inspect client/realm events read-only,
   without exporting account/session identifiers, packet payloads or credentials. Do not change
   or restart the server. Do not blame perf, anti-cheat, renderer failure or server failure without evidence.
2. A new sample needs continuous numeric world-active markers (or equivalent verified scene
   evidence), so transitions/logout can be classified rather than guessed from draw counts.
   Adding telemetry is a separate code change requiring the usual approval/tests.
3. Preserve existing logs before relaunching a reused label. The isolated run's `Data` link is back
   to `/home/arlyon/Games/WOTLK/Data`; the user restored the archives there. A temporary overlay
   is unused. Do not move shared archives or copy account configuration.
4. Separate profiling build: `out/client-vulkan-profile/bin/Wow`, expected hash
   `a35647cea14bad8817b28a3faa48091a4121d55d45a58bd02a9ab8430e5a1a87` for this capture.
   Flags: `-O3 -DNDEBUG -gline-tables-only -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`;
   1390 C/C++ translation units audited. Rebuild/re-audit if source changes; keep normal build separate.
5. User explicitly approved temporary focus for profiling, with previous focus restored afterwards.
   Interactive runs must NOT enable validation. Capture uses `cycles:u`, 199Hz, FP return-address
   chains and monotonic timestamps. No DWARF stack-memory dumps or raw game-memory capture.
6. Obtain matched scene/settings/view/foreground data on normal and modified builds; report
   median/p95/p99, CPU/GPU/waits, uploads and residency. Low GPU utilisation alone is not proof.
7. Core/synchronisation validation belongs in separate diagnostic GPU fixtures. Preserve working
   live UI scaling/resize, circular minimap masking, clouds and DXVK reference behaviour.

The reviewed local FlameGraph source is pinned at `41fee1f99f9276008b7cd112fca19dc3ea84ac32`
under `out/deps/flamegraph-41fee1f/`. SVGs are offline and use no external browser resources.

## Canonical validation (from this worktree)

```sh
cmake --build out/client-vulkan --target Wow --parallel 4
cmake --build out/portability-tests --parallel 4
ctest --test-dir out/portability-tests --output-on-failure
cmake --build out/vulkan-tests --parallel 4
ctest --test-dir out/vulkan-tests --output-on-failure
git diff --check
```

Last completed renderer bundle: full client build, 41 portability tests and four Vulkan CTest
registrations passed. Re-run after code changes. Residency has synthetic GPU/lifetime validation;
controlled world traversal/memory verification and matched DXVK baseline remain open.
