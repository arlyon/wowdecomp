# 4.3.4 renderer donor inventory

Status: initial source inventory, not an implementation approval or performance claim.

## References and boundaries

- Local 3.3.5 reconstruction: repository HEAD `4a293adc` (platform additions are present). Source references below use the root checkout; unrelated working changes were not modified.
- Local 4.3.4 reconstruction: `../wowdecomp15595`, HEAD `959b0b85`.
- Separate modern 3.3.5 reference: `.wt/rosemyne-preview`, RosemyneH/wowdecomp `140e6e5e39a66280b49ac819ae456c9226eaef86`.
- Current independent-presentation experiment: `.wt/renderer-split`; it is not the untouched 3.3.5 baseline.

This compares reconstructed source trees. Different names, files, address-tag coverage and platform additions are not automatically original Blizzard architectural changes. In particular, the 15595 README distinguishes original addresses from carried-over 12340/13623 implementations. Counts below measure source organization, not completeness, speed or working gameplay.

## Summary

The substantial donor is the **world-rendering frontend**: per-view visibility/render state, world-map gathering, explicit instancing and framebuffer/depth contracts. D3D11 is one backend supporting those contracts, not the entire change.

Neither an independent high-frequency UI loop nor a broad animation job pool follows automatically from 4.3.4. Its checked-out `CGWorldFrame::RenderWorld()` still invokes world update and render synchronously. Its M2 animation path retains the older main-thread/one-worker split and join.

## Inventory

| Area | Evidence in 15595 | Difference / potential value | Risk and qualification |
| --- | --- | --- | --- |
| Per-view world state | `WoW/Source/WorldClient/WorldView.h`, `WorldView.cpp` | `CWorldView` owns camera matrices, frustum/portal stacks, occlusion, visible chunks/groups and parent-view identity. The scene invokes explicit `Gather()`, `Cull()`, `Render()` phases. Better ownership boundaries for multiple views and eventual bounded preparation. | Still mutable state and pointers into the live world. It is not an immutable worker request. Much older state lived on `CWorldScene`; count growth is not proof every algorithm is new. |
| World-map gathering | `WorldMap.h`, `WorldMapGather.cpp`, `WorldMapUpdate.cpp`, `WorldMapPreload.cpp` | `CWorldMap::Gather(view)` selects entities, WMO definitions, areas and low-detail geometry through view flags. Distinct map/view responsibilities are useful transplant seams. | Broad dependencies on map loading, entities, queries and formats. Not a drop-in replacement for 3.3.5 `CMap`. |
| Scene/pass orchestration | `WorldScene.cpp:932`, `Ui/WorldFrame.cpp:2685` | Scene gathers/culls the default view, animates/begins M2 drawing, calls render callbacks, prepares reflection views and renders the main view. World-frame opaque/transparent work plugs into callbacks. | More explicit sequencing, not asynchronous UI. Reflection/depth/extra passes can add GPU cost. |
| Explicit instancing | `Gx/CGxDevice/CGxDevice.h:194`, `Gx/CGxDeviceD3d11/CGxD3d11State.cpp:969`, `Model2/M2Scene.cpp:2479` | Explicit indexed/nonindexed instanced draws; `m_doodadCounts` groups model elements for rendering. A strong candidate for reducing repeated mesh submission and per-instance vertex work. D3D9 also implements the instancing contract. | Must preserve compatible materials, alpha sorting, lighting, instance transforms, skinning and collision. Needs parity and draw/upload-count measurements. Not every model batch is safely instanced. |
| Terrain geometry/batches | `MapChunkBuf.h`, `MapChunkBuf.cpp`, `MapChunkRender.cpp`, `MapRenderChunk.cpp` | Separate chunk buffer ownership and render routines, layer batches and shader-specific terrain paths. Worth comparing allocation, rebuilding and submission costs. | Existing 3.3.5 terrain already has batching/pools. Do not claim all batching is new or more efficient without an algorithm comparison. ADT/material assumptions must remain Wrath-compatible. |
| Framebuffer abstraction | `Gx/CGxFrameBuffer.h`, `Gx/CGxDevice/CGxDevice.h:168` | Explicit attach/finalize/set/back-buffer operations instead of only the old color/depth target pair. Enables clearer multi-target, reflection and depth ownership. | New Gx contract affects every backend and restoration path. Our Vulkan HDR/TAA/cached-world paths already implement some equivalent functionality privately. Avoid duplicate abstractions. |
| MRT and formats | `Gx/Types.h` | Local Gx format enum grows 13→24; target enum grows from color/depth to depth plus four color slots. Adds float/depth/vendor-depth entries. | Enum values change: depth/color ordering reverses, texture/vertex IDs shift. Raw numeric compatibility is unsafe. More formats do not by themselves improve performance. |
| D3D11 backend | `Gx/CGxDeviceD3d11/` (~4,900 lines) | Device/swapchain, state objects, shader binding, resources and buffer streaming. Stream maps use `WRITE_NO_OVERWRITE` or `WRITE_DISCARD` on wrap. | Windows API backend. Current native Vulkan already avoids the D3D9 driver path. Inspected code uses an immediate context; no deferred-context/command-list recording was found in this backend. Build/run parity is not established here. |
| Shader/profile expansion | `Gx/Types.h`, `Model2/M2Shader.*`, `ShaderEffect/DefaultShaders.*`, `Model2/M2Scene.h` | Explicit SM4/SM5 profiles, expanded shader plumbing and hull/domain permutations in model elements. | Some stage names already exist in the local 3.3.5 enum and may be reconstruction carryovers. Presence of a name is not a new implemented feature. Shader binaries/permutations/material semantics cannot be substituted blindly. |
| M2 scene organization | `Model2/M2Scene.h`, `M2Scene.cpp` | Pass arrays grow 3→6; adds preload records, a recycle list, instance counts and scene lifecycle fields. Candidate improvements in draw organization and loading/retirement. | Requires tracing actual pass semantics, model lifetimes and asset dependencies. This alone does not establish less CPU skinning or faster animation. |
| M2 threading | `Model2/M2Scene.cpp:2001`; compare 12340 `M2Scene.cpp:1687` | Both paths start the cache worker, process alternating root models on the caller, wait for the worker, then perform serial animation/lighting work. | **Not a newly discovered general job system.** A port of this path alone is not the answer to our owner-thread stalls. |
| Depth/effects | `ShaderEffect/DepthCache.*`, `SunShafts.*`, `FFX/PassWaterWindow.*` | Explicit sampled/resolved depth handling, sun shafts and water-window postprocessing. Potentially useful depth-consumer contracts and feature parity. | Primarily rendering features/correctness, not performance fixes. Added passes and bandwidth can make the GPU bottleneck worse. |
| Liquids/reflections | `WorldClient/WorldScene.cpp:932`, `WoW/Source/LiquidSystem/` | Real/fake reflection views and framebuffers, procedural/displacement textures and material settings. | Significant visual/data/DB coupling. Not a necessary first step for CPU or input latency. Preserve original 3.3.5 liquid rendering until independently validated. |
| Lighting/sky organization | `WoW/Source/DayNight/` | Separate sky/cloud/star/planet/zone-light and streaming implementation. | Many former WorldClient files moved into this module. Their disappearance from WorldClient is not removed functionality. Validate aesthetic changes separately from performance. |
| Frame timing | `Engine/Source/Event/FrameTime.*` | Timing ownership moved out of the old WorldClient location. Worth tracing alongside real UI/world/present metrics. | A location change is not proof of a better clock or independent scheduling. |

## Hard compatibility evidence

`Engine/Source/Model2/M2Shared.cpp` accepts different model versions:

- 3.3.5: exactly `0x108`.
- 15595: `0x109` through `0x110`.

Therefore, importing the 15595 model loader wholesale would reject stock Wrath models. The preferred boundary is **Wrath asset decoding → explicit renderer-facing data**, not forcing all assets to Cataclysm formats.

The Gx color/depth slot ordering and numerous numeric enum values also change. Any adaptation must use semantic conversions rather than copying integers or binary layouts.

## Source scope (illustrative only)

| Module | Local 3.3.5 files | 15595 files | New relative names |
| --- | ---: | ---: | ---: |
| Gx | 53 | 77 | 29 |
| Model2 | 17 | 22 | 5 |
| ShaderEffect | 9 | 15 | 6 |
| FFX | 12 | 20 | 8 |
| WorldClient | 112 | 107 | 38 |

Only `.cpp`, `.h` and `.inl` are counted. Reorganization is substantial: WorldClient also loses 43 relative filenames, some moved elsewhere. These are not patch-size estimates. The local 3.3.5 tree includes custom platform work such as D3D9Ex; its absence in the 15595 tree is not automatically a Blizzard removal.

## Keep Rosemyne's changes separate

At the pinned Rosemyne revision, the most relevant custom changes are:

- General `Job::ParallelFor` and `Start`/`Wait` pool for selected CPU work.
- Parallel M2 root animation, CPU-skinning fallback, terrain/detail vertex filling and doodad culling.
- Persistent rigid/bind-pose meshes and GPU bone palettes, with CPU fallback.
- Visibility/layer-plan reuse and persistent bindless terrain slots.
- Profiling zones, including main-thread job-join cost.

Primary references: `docs/performance.md`, `docs/graphics.md`, `Engine/Source/Base/Job/JobSystem.*`, `Engine/Source/Model2/M2Scene.cpp` in that checkout.

These are **custom 3.3.5 mechanisms**, not proof of stock 4.3.4 behavior. Its Vulkan renderer explicitly does not load retail `.bls` shaders and keeps `CShaderEffect` disabled, using fixed-function-style GLSL instead. Its observed speed cannot be credited exclusively to threading/caching while assuming shader/effect parity with our renderer.

## Mapping to our measured problems

| Current problem | Most relevant exploration | What remains unproven |
| --- | --- | --- |
| Owner world preparation/recording tens of milliseconds | Instance grouping; retained meshes; terrain rebuild/submission audit; per-view gather boundaries | Which portion of our actual CPU profile is saved and whether UI p95 improves |
| Interaction capture commonly ~14ms, with larger spikes | Separate immutable actor/geometry ownership and on-demand query work | Neither the stock D3D11 backend nor its M2 worker removes our added capture cost |
| RT world GPU work ~40–46ms | Independent profiling of receivers, AS work, pass cost and submission granularity | A new backend/API does not preempt the shared queue or shorten the existing shaders automatically |
| Cursor/UI cadence below 120Hz | Shorter owner work, bounded GPU scheduling and actual presentation/input instrumentation | Neither donor establishes a 120Hz UI guarantee |
| Maintain rendering quality | Preserve Wrath shader/material semantics; stage features separately | Rosemyne's simplified shader path is not parity evidence |

## Recommended next exploration slices

1. **Instancing vertical slice:** trace 15595 grouping keys → instance stream → shader semantics → draw call; compare the exact 12340 path. Produce a compatibility matrix for rigid, skinned, alpha-tested and blended batches, plus a baseline draw/upload cost breakdown.
2. **View ownership slice:** map which fields move from old static scene state into `CWorldView`, what still aliases live map/model/Lua state, and the smallest Wrath-compatible gather/cull result boundary. Do not assume copying the class makes it thread-safe.
3. **Terrain work reuse slice:** identify allocations, visibility reconstruction, layer grouping and buffer rewrites that can be reused without changing appearance or camera correctness. Compare Rosemyne caching separately.
4. **Backend contract slice:** specify only the instancing/framebuffer/depth capabilities required by the first measured improvements; implement them in native Vulkan if a later plan is approved. A Windows D3D11 transplant is not a prerequisite.
5. **Quality expansion later:** liquid reflections, new postprocess effects and newer shader profiles should have their own parity/data gates, rather than increasing the cost of the first latency experiment.

## Validation/decision gates before implementation

- Pin donor/base commits; preserve unrelated root and renderer work.
- Profile the same camera, actors, settings and GPU in Release, with RT on/off reported separately.
- Measure wall-clock presents, input-to-update latency, owner preparation, capture, GPU time, draw count and uploaded bytes. The Lua FPS counter is not presentation evidence.
- Require deterministic image/material/camera/picking parity for each selected mechanism, including moving cameras, animation, alpha, loading and resize.
- Preserve one logical device, the native Linux/Vulkan path, original game/Lua ownership and a reversible fallback.
- Do not relax rendering quality or move mutable engine state to workers merely to hit a timing target.
- Retain the known terrain-status failure as a failure until it is fixed; do not exclude it.
- The inventory alone establishes no performance improvement; implementation/evidence for the subsequently approved narrow slice follows.

## Approved first implementation: retained detail-clutter instancing

The user approved detail-doodad hardware instancing while retaining the independent UI/world split. This is not a whole-renderer transplant, a `CWorldView` ownership refactor, or an M2 asset-format port.

### Delivery contract

- Keep one logical device, original world owner/producer/consumer scheduling, completed-image picking and synchronous rollback unchanged.
- Default-off, non-persistent A/B CVar: `/console nativeDetailInstancing 1`; `0` restores the legacy path. No new Video Options control. Invalid values preserve the current value.
- Retain the original Wrath skin's vertices/indices once per detail model and supply one 80-byte placement/normal/colour record per instance. Unlike the donor's vertex instance stream, native Vulkan uses a vertex-stage storage buffer indexed by `SV_InstanceID`; there is no new shader model or material replacement.
- Preserve the recovered Wrath vertex program after placement (including authored wind/fade/fog/lighting), pixel program, texture ordering and alpha/depth state. Only homogeneous, multi-instance batches with a calculated geometry-byte saving qualify; mixed/single/unprofitable batches retain the old path.
- Native capability/shader failure and temporal-AA rendering fall back to legacy detail rendering. Do not change requested AA/HDR/RT settings. HDR remains eligible. AA instancing is explicitly deferred rather than supplying incorrect motion vectors.
- Retained mesh and instance buffers participate in the existing resident-buffer versioning and producer retirement. Map/model/batch teardown releases caches. No mutable game state is accessed by the submission worker.
- Count eligible draws, instances, mesh/instance build bytes, legacy-equivalent bytes, fallbacks and mesh/instance CPU build time. Byte reduction is not proof of lower draw count, reduced end-to-end frame time or 120Hz UI.

### Validation seams and evidence

`VulkanShaderTest` compiles the recovered-program wrapper and rejects unrecognised input ABI. `DetailDoodadInstancingGpu` and `DetailDoodadInstancingRtGpu` run the actual client world/detail producer with stock assets, original textured UI and image-matched interaction. Both verify default/invalid toggles, geometry-byte reduction, full-scene colour/depth, AA fallback, HDR eligibility and grass-only colour/depth at two camera positions. The isolated oracle prevents the terrain/background from hiding missing clutter. Original source meshes and per-batch instance buffers are destroyed while the producer is held, followed by twenty 500ms holds, publication/recovery, resize/invalidation and synchronous rollback.

Measured first eligible Goldshire generation: 10 draws / 43 instances; retained mesh 408 bytes + instance records 3,440 bytes versus 17,544 legacy-equivalent bytes (78.1% less build data for eligible geometry only). This does not mean 78.1% less total world upload/work. Both grass-only views (189 and 333 covered pixels) matched exactly in colour/depth. Full-scene comparison had nine significant-colour pixels and three depth-edge pixels, mean RGBA error 0.000670; it is not a claim of bitwise whole-scene equality.

Full 63-test suites pass 62/63 in canonical/core/synchronization modes, with no Vulkan diagnostics or skips. The sole failure remains the previously reproduced `TerrainDirectionalFallbackGpu`: `RT-off did not restore disabled status`. Do not waive it. Native Vulkan and Vulkan+DXVK client builds and all 57 portability tests pass. Final evidence under `out/checks/`: `detail-delivery-canonical-1792540944974959157`, `detail-delivery-core-1792541088951426896`, `detail-delivery-sync-1792541291032538187`; each `results.json` records CTest exit 8, 63 tests, no diagnostics/skips. Canonical first-build samples: RT-off mesh 0.004719ms / instances 0.055364ms; RT-on mesh 0.003367ms / instances 0.042619ms. These are small-scene preprocessing samples excluding shader compilation/GPU execution, not comparative CPU speedup evidence. Build logs: `detail-final-{client,both,tests}-build.log`; portability `detail-portability.log` (57/57); final `git diff --check` clean. Live gameplay/streaming coverage and an end-to-end performance improvement remain unproven; held-producer UI throughput is not gameplay latency evidence.
