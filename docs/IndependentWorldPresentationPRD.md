# Independent UI and World Presentation — Delivery PRD

**Status:** approved for execution by the user's “go”; recording resources and completed-image publication/reuse are implemented and tested in the native backend. Original `WorldFrame` scheduling, independent producer recording and image-matched interaction remain pending. See checkpoints 11–12.
**Mode:** full, from context: this changes threading, GPU submission, resource lifetimes and displayed-frame interaction contracts.
**Baseline:** commit `da4dce90`; implementation branch `renderer-split` in `.wt/renderer-split`. The original `.wt/renderer` worktree is a separate receiver workstream and is not a clean baseline reference.
**Owner:** independent UI/world scheduling agent. M2/WMO shadow receivers are a separate, concurrently developed workstream.

## 1. Problem and evidence

UI presentation currently waits for world production. In `WoW/Source/Ui/WorldFrame.cpp`, `CGWorldFrame::RenderWorld()` runs `OnWorldUpdate()`, `OnWorldRender()` and world text rendering synchronously inside a queued UI render callback. `Engine/Source/Frame/CSimpleTop.cpp::OnLayerRender()` executes the strata batches containing that callback. The native Vulkan backend shares frame command recording and a completion fence between world rendering and UI presentation; `State::BeginFrame()` waits for that fence before reusing frame resources.

The user has confirmed working live terrain RT shadows, including character casting, but reported approximately 1.5 world FPS. A bounded process sample showed approximately 49% of one CPU core and 52% graphics-engine utilisation; those observations do not establish the cause. The preview has used both 1422×1854 and 2859×1854 drawables. Release optimisation is enabled. CPU/GPU phase timing instrumentation exists in the baseline but its GPU timestamps have not yet been runtime-validated.

The independent-cadence requirement is distinct from fixing world throughput: menus, cursor-driven UI and input processing must not wait for the next world image. This work MUST NOT claim that decoupling alone fixes world FPS.

## 2. Outcome and non-goals

### Outcome

One existing logical Vulkan device and one presentation swapchain support two independent lifetimes:

- World production renders a complete generation into an offscreen target.
- UI presentation composites the newest compatible **completed** world generation, draws current UI and presents without waiting for an unfinished generation.

The world producer can be idle, CPU-stalled or preparing another generation while UI presentation continues. Displayed-world picking uses the camera and interaction data belonging to the displayed image.

### Non-goals

- A second logical Vulkan device, cross-device memory/semaphore FD transfers, a second game simulation, animator, material system or renderer implementation.
- Restoring the archived executor wholesale, including its obsolete RT status/receiver machinery.
- M2/WMO lighting/shadow receiver implementation, GI, volumetric fog, low-resolution shadow reconstruction or temporal shadow accumulation.
- A general task graph, job-system rewrite, arbitrary render-command protocol or unbounded frame queue.
- Promising responsive UI behind an arbitrarily long, non-preemptible GPU submission. The queue limitations must be measured and disclosed.
- Shortening world draws, splitting existing world GPU submissions or adding GPU preemption. The user explicitly deferred these optimizations: this work breaks the requirement to produce a new world render for every UI frame; residual GPU contention is acceptable.
- Changing existing RT/HDR/AA compatibility policy or silently changing saved quality settings.

## 3. Requirements and acceptance

| ID | Requirement | Acceptance |
|---|---|---|
| R1 | Exactly one logical Vulkan device and one presentation swapchain MUST own world and UI resources. | Instrumented lifecycle/device ownership proves one `VkDevice`; no external-memory/semaphore FD transport or second device factory is introduced. Device recreation never leaves two live renderer devices. |
| R2 | World production and UI presentation MUST have distinct reusable recording/completion resources. | Pausing world production does not make UI wait on a world fence, reset its command pool or mutate its descriptors. Validation reports no concurrent recording, reset-in-use or resource lifetime errors. |
| R3 | UI MUST consume only completed, compatible world generations and MUST NOT wait for an unfinished one. | A deterministic 500 ms CPU-side producer hold before GPU submission leaves UI/input progressing. At 1280×720, vsync off, on the pinned Radeon target, measure over 10 seconds after warm-up: at least 30 UI presents/second, UI present-interval p95 ≤50 ms, and synthetic input-to-UI-update p95 ≤50 ms. Report vsync-on behaviour separately. |
| R4 | Production MUST be bounded, latest-completed and free of accumulating obsolete work. | A fixed slot set—initially three unless evidence justifies otherwise—bounds image memory and outstanding generations. A busy producer is not given a growing queue. After a hold is released, obsolete requests are coalesced instead of replaying a backlog. |
| R5 | Original game/model/UI ownership MUST remain race-free. | The producer cannot access mutable Lua, SDL/window state, global device state, model animation or live material state concurrently without an explicit ownership hand-off. Every worker-consumed resource is immutable for the generation and retained through GPU completion. Test mutation/deletion of source resources while an older generation remains displayed. |
| R6 | UI-thread work required to request a world generation MUST be bounded and measured. | Separately record request/snapshot CPU time. The R3 input/presentation criteria must still pass while producing generations, not only while re-presenting an already captured image. Moving GPU recording off-thread while retaining a long synchronous capture is not completion. |
| R7 | Displayed-world interaction MUST match the displayed generation. | With a held old image and a changed current camera/object pose, picking selects according to the old displayed camera/geometry; UI hit testing remains current. Deleted object IDs resolve safely to a miss. No picking snapshot is reused across a scene/lifecycle epoch. |
| R8 | Resize, loading, map/camera-owner changes, logout and shutdown MUST safely invalidate incompatible generations. | Resize never samples mismatched extents or uninitialised depth; old scenes cannot reappear after a new scene begins. No completed image produces a defined cleared/loading background plus functioning UI. Shutdown joins/finishes owned work before destroying resources; no deadlock, use-after-free or unbounded wait caused by an unsignalled application-controlled dependency. |
| R9 | GPU submission and completion MUST obey the actual device's queue capabilities. | Queue submission/presentation is externally synchronised. The UI never enqueues a wait for unfinished world output. Report UI intervals, world submission GPU durations and queue topology. Existing world draws/submissions MAY still delay UI execution through shared-queue GPU contention; shortening or splitting them is deferred, not an acceptance blocker. A second queue is optional, queried and not treated as proof of independent hardware execution. |
| R10 | Completed world publication MUST preserve original output and lighting semantics. | Actual-world raster off-path comparison and terrain RT fixture checks pass; compositing consumes the final world colour after effects/fallback. Publication includes only valid world viewport pixels and required depth/metadata, without overwriting current UI or leaking another generation's state. |
| R11 | Off/unsupported/failure paths MUST be explicit and reversible. | Existing synchronous rendering remains available. Requesting unavailable independence retains synchronous rendering with an exact diagnostic; it cannot display stale generations as current or silently disable RT. Saved quality intent remains unchanged. |
| R12 | CPU/GPU/resource cost and UI/world cadence MUST be observable separately. | Deduplicated state-transition diagnostics identify active/fallback mode and reason. Rate-limited performance output reports UI presents, produced/displayed world generations, generation age, CPU preparation/recording, GPU world/composition intervals and slot/memory usage. No per-draw or per-frame log flood. |

**GPU scheduling boundary — user clarification:** success is removing the dependency on a new world render for each UI frame, not making GPU execution independent. R3 remains a controlled CPU/lifecycle-independence test; its thresholds are not an unconditional promise under production GPU contention. UI may reuse the latest completed compatible world image while production continues. Existing GPU draws/submissions may still delay UI execution, and shortening those workloads is a later workstream, not a blocker for completing this split. Measure and disclose the limitation; do not introduce waits for unfinished world output or hide contention by silently lowering quality.

## 4. Architecture and contracts

### Execution shape

```text
Original game/UI owner
  input + UI updates + original simulation/animation ownership
             │ bounded immutable world request
             ▼
World producer, same VkDevice
  isolated recording resources → offscreen world generation → completion
             │ publish colour + interaction + lifecycle metadata together
             ▼
UI presentation owner
  select latest compatible completed image → composite → current UI → present
```

A world request is not permission to duplicate original simulation or to move unreviewed engine globals onto a worker. Inspect the actual ownership/cost boundary first. Reuse narrowly useful archived snapshot code only when its assumptions remain valid.

### Minimum generation identity

A published generation carries:

- Lifecycle/scene epoch and monotonic generation identifier.
- Output extent and initialised world viewport rectangle.
- Retained final colour and any depth genuinely required by current consumers.
- Camera/projection/viewport and retained interaction snapshot matching the image.
- A completion token, retained resource ownership and output/lighting status.

Presentation is not allowed to assemble these fields from different generations. Avoid recreating unrelated RT transport/status structures from the archived design.

### Slot lifecycle

Each slot has an explicit equivalent of `available → producing → complete → displayed → retired/available`. A displayed slot is never overwritten while a UI submission samples it. Production and UI consumption completion are tracked separately. Image visibility/layout barriers and queue-family ownership transfers, if needed, must be explicit. Prefer existing fences or queried/enabled timeline semaphores; do not add a capability-dependent path without an unavailable behaviour.

The UI may retain the last valid image while a newer frame is unfinished. That is intentional reuse, not a claim that its camera or lighting is current. Incompatible epoch/extent invalidates reuse.

### Synchronisation constraints

- Separate command pools and per-flight transient descriptors/upload storage for independently recorded work.
- No concurrent access to the existing `State` recording fields or legacy global shader constants.
- No broad mutex held while waiting for GPU completion, joining a worker or presenting.
- Queue handles must be externally synchronised, even with multiple recording threads.
- Never put a wait for unfinished world work ahead of UI on a shared queue.
- Keep resources alive until both producer and all presentation consumers finish.
- No synchronous readback or device-idle operation in the normal presentation loop.

## 5. Existing seams and preserved reference

### Active implementation to inspect

- `WoW/Source/Ui/WorldFrame.cpp`: callback ownership, world update/render ordering, world text, projection restoration, effects/fallback completion.
- `Engine/Source/Frame/CSimpleTop.cpp`: strata batching and current UI execution.
- `Engine/Source/Gx/CGxDevice/CGxDevice.h`: existing world/output hooks.
- `Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.{h,cpp}`: `State::BeginFrame`, `Submit`, presentation, world targets, shared resource lifetime.
- `WorldTarget.h`, `HdrScene.inl`, `TemporalResolve.inl`, `WorldShadows.inl` in the native backend: reuse existing boundaries; preserve existing capability/rollback semantics.
- `WoW/Source/UIUtil/Camera.cpp` and original picking/interaction paths: identify actual displayed-frame consumers before altering them.

### Archived experiment: reference, not an approved implementation

Relative to the repository root, preserved files are under:

`out/renderer-reset/evacuated-originals/vulkan-ui-distance-1792334381405103061/contents/`

Relevant names include `WorldRuntime.inl`, `WorldMailbox.h`, `WorldCapture.inl`, `WorldCommands*`, `World*Snapshot.h`, `SharedWorld*`, `WoW/Source/Ui/WorldFrame.cpp` and `Tools/Vulkan/WorldMailboxTest.cpp`.

This experiment creates an offscreen `CGxDeviceVulkan` and calls `InitializeDevice` again, then captures/replays commands and transfers output between devices. Its two-device design, external-handle transfers and old RT dependencies are explicitly **not** the target. The files were preserved as archived work; do not assume a standalone cherry-pick exists. Do not alter or clean the archived originals.

## 6. Parallel work and integration boundary

The other agent will implement M2/WMO shadow receiving/self-shadowing and may extend existing shared directional shaders, authored source selection, original lighting provenance and graphics descriptor bindings. This PRD's owner MUST NOT implement or replace those systems.

- Use an isolated worktree branched from `da4dce90`, under `./.wt/<name>` relative to the working directory, following repository instructions.
- Before editing shared backend files, identify the scheduling-owned fields/methods and communicate the boundary to the receiver workstream.
- Expected overlap: `CGxDeviceVulkan.{h,cpp}`, `CGxDevice.h`, `WorldFrame.cpp`, GPU fixture registration. Commit small cohesive scheduling changes; never replace a whole shared file from an old lane.
- World production continues to call the **current** renderer/shadow preparation and receiver path. No private fork or copied receiver implementation in a worker.
- RT scene/AS/material resources are retained by their actual production generation until its GPU work completes. UI re-presentation samples completed colour; it must not rebuild or mutate RT geometry.
- Rebase/merge integration must rerun actual-world and receiver checks with both changes present. A split that works only with RT disabled is not accepted.
- Single-layer terrain receiver identities `vs_3_0/Terrain/8` and `ps_3_0/Terrain1/0` were observed missing after the baseline's live demonstration. Preserve truthful fallback; do not weaken readiness to make integration tests pass.

## 7. Validation plan

Paths below are relative to the repository root used in this session. In another worktree, use that worktree's configured build directories and existing dependency configuration; do not install/download dependencies or run the DXVK build script.

| Requirement | Seam / evidence | Negative case |
|---|---|---|
| R1–R2, R4–R5 | Focused ownership/slot tests and Vulkan validation. Count actual device creation and retained slots/resources. | Busy slots, concurrent source mutation, retired generation, resource destruction before UI completion. |
| R3, R6 | Real UI event/render fixture with a deterministic 500 ms producer hold and wall-clock present/input timestamps; repeat while requests are being produced. | A worker-only test or a blank window that does not exercise original UI callbacks is insufficient. |
| R7 | Original world interaction integration fixture with deliberately different displayed/current cameras and model poses. | Scene transition, removed object, stale viewport, old publication racing cancellation. |
| R8, R11 | Resize/minimise/restore, loading/logout/map change, unsupported capability and producer failure tests. | Pending output on shutdown, no first completed frame, allocation refusal, repeated toggles. |
| R9, R12 | Same-device bounded GPU workload fixture, actual queue topology, CPU/GPU timings and cadence report. | Head-of-line blocking, accidental waits on the producer, misleading CPU-only claims. |
| R10 | Existing actual-world terrain/raster/RT tests plus manual live client on the combined branch. | Mixed-generation colour/depth/interaction, missing source, late Classic fallback, effects ordering. |

Existing commands/checks:

```sh
cmake --build .wt/renderer-split/out/client-vulkan --target Wow --parallel 4
cmake --build .wt/renderer-split/out/client-both --target Wow --parallel 4
cmake --build .wt/renderer-split/out/portability-tests --parallel 4
ctest --test-dir .wt/renderer-split/out/portability-tests --output-on-failure
cmake --build .wt/renderer-split/out/vulkan-tests --parallel 4
python3 .wt/renderer-split/out/checks/run-checks.py independent-world-final --modes canonical core sync
```

The runner above is a worktree-local adapter to inspected, session-local preserved tooling outside the consolidated branch. It changes only the original runner's hard-coded build/output root to `.wt/renderer-split/out`; inspect both files before reuse. On a fresh build, build the native client before configuring the Vulkan fixtures: their world archive list is globbed at configure time. It serialises GPU use through `out/renderer-gpu-validation.lock`, selects the Radeon driver and records actual validation-layer evidence. Do not run competing GPU fixtures while the live preview owns the lock. Never stop a user's client without approval.

Existing targeted test names include `NativeVideoOptions`, `TemporalLiveGpu`, `TemporalUpscaleLiveGpu`, `SharedDirectionalQueryGpu`, `RasterWorldFeaturesGpu`, `TerrainDirectionalSliceGpu`, `TerrainDirectionalFallbackGpu` and `TerrainDirectionalPressureGpu`. Add new scheduling tests through the existing CMake/CTest conventions; their names are implementation output, not pre-existing commands. Run diagnostics/targeted tests before the full suite. Preserve failure evidence and report unavailable validation honestly; no broad suppression or weakened assertion.

The default hardware target is the Radeon 890M/RADV with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json`. The user may separately test an RTX 2080 Ti. Do not assume that GPU is attached, change device selection automatically or claim unperformed NVIDIA validation.

## 8. Rollout, observability and rollback

Introduce explicit opt-in scheduling behaviour through the existing Video Settings integration, with Apply/Cancel/Defaults semantics and exact unsupported/failure messages. Keep the synchronous path available throughout delivery. Do not add console-only user requirements or silently enable HDR/AA/change resolution to improve benchmark numbers.

Publish active mode, fallback reason and separate UI/world cadence without claiming world FPS improvement. Any runtime fallback must withdraw incompatible output/interaction together and avoid blending independent and synchronous ownership for one generation. Rollback disables independent scheduling and returns to the original synchronous path; no data migration or external service changes are required.

## 9. Risks and decisions before implementation

1. **Main-thread capture/preparation cost:** measure it before choosing the packet boundary. R6 forbids hiding a long synchronous capture behind a worker.
2. **GPU head-of-line blocking:** one logical device is mandatory, but queue count and useful overlap are hardware-dependent. Establish the target's capabilities; do not assume another queue solves scheduling.
3. **Original mutable/global ownership:** identify exact consumers; no global pointer swapping while another thread renders.
4. **Displayed-frame interaction retention:** bound memory and lifetimes without retaining live mutable world objects unsafely.
5. **Feature integration:** effects, scene scale, existing AA/HDR fallback, world text and new receivers must all consume correct generations. Unchanged compatibility restrictions are not bugs to fix by silently enabling partial output.
6. **Parallel shared-file edits:** communicate boundaries and integrate incrementally; no duplicate receiver or descriptor ownership.

Before code changes, present the discovered request/ownership boundary, queue plan, slot/completion contract and any unavoidable deviations from these requirements. Ask only for consequential unresolved choices; do not reopen unchanged agreed behaviour.

## 10. Delivery sequence and definition of done

1. Audit current owner/callback/fence paths and validate the minimum same-device ownership design. Record the CPU capture cost and actual queue capabilities.
2. Separate reusable world and presentation resources; establish completed-image publication with bounded slots, initially without changing lighting.
3. Add the producer hand-off and nonblocking UI composition; prove R3/R6 with original UI under a deterministic producer stall.
4. Bind displayed interaction and lifecycle cancellation; prove R7/R8 and failure fallback.
5. Integrate current RT and concurrent M2/WMO receivers; run targeted then canonical validation, capture live evidence and publish remaining scheduling/performance limits.

Done means the mapped acceptance criteria pass on the combined implementation with retained evidence, not merely that a worker runs, a stale image is visible or the client builds. No claim that the umbrella renderer PRD, RT performance, GI or fog is complete follows from finishing this workstream.

## 11. Historical checkpoint — recording resources only

This records the first implementation increment; section 12 supersedes its implementation status and test totals.

### Implemented

- `RecordingResources.h` isolates command pool/buffer, completion fence, transient descriptor pool, mapped upload arena/cursor and transient framebuffers on a borrowed `VkDevice`. It has no device factory, queue, swapchain, external-handle transport or engine-global access.
- The existing synchronous backend now uses that resource lifetime, preserving the original rendering/lighting path and its single device. Nonblocking `TryReset()` refuses incomplete GPU work; the synchronous path still deliberately waits as before.
- `RecordingResourcesGpu` exercises two lanes on one device, preserving producer descriptors and output while the other lane completes 16 submissions during a deterministic >=500 ms pre-submit CPU hold. It also tests pending-GPU reset refusal, idempotent teardown, retry after partial initialization failure and invalid-state rejection. This is **not** an original-UI, presentation-rate or input-latency test.
- No worker, completed-image slots, user-visible opt-in, displayed interaction snapshot or frame-rate independence is active yet. R2–R12 are not complete.

### Observed hardware and ownership

- Pinned RADV target: AMD Radeon 890M, Mesa 26.1.8. Selected graphics/present family 0 exposes **one queue**; the fixture requests that one queue on one logical device. This does not establish that another family supports the required graphics/presentation work or that useful GPU overlap exists.
- `CGWorldFrame::RenderWorld()` still performs `OnWorldUpdate()`, names, `OnWorldRender()` and world text synchronously within the strata callback. Update touches live objects, animation, audio and world state. It cannot be moved wholesale to a worker.
- Archived capture explicitly rejects same-device execution and imports obsolete RT/device-transport machinery. It remains reference-only.
- CPU request/snapshot cost and actual UI cadence have not been measured. The next design step must identify and measure an immutable handoff boundary; a long owner-thread capture is not accepted as decoupling.

### Validation evidence

All artifacts are under `.wt/renderer-split/out/checks/` (ignored, local evidence).

| Check | Outcome |
|---|---|
| Compile-command-derived syntax diagnostics on backend and new fixture; fixture additionally checked with `-Wall -Wextra -Werror -Wno-missing-field-initializers` | Passed |
| Native Vulkan `Wow` build | Passed |
| Vulkan + existing DXVK `Wow` build, reusing installed dependency outputs | Passed |
| Portability build and CTest | 57/57 passed |
| New GPU fixture, canonical/core/sync validation | Passed in each mode; validation-layer insertion verified and no Vulkan diagnostics |
| Complete Vulkan fixture build | Passed after reconfiguring once client archives existed |
| Complete CTest suite, canonical/core/sync, no test exclusions | **45/46 passed in each mode**; one baseline failure, zero Vulkan validation warnings/errors |
| Exact `da4dce90` backend rebuilt into an isolated baseline world fixture | Reproduced `TerrainDirectionalFallbackGpu`: `RT-off did not restore disabled status` |

Full-suite logs: `recording-isolation-final-1792507508131893028/canonical.log`, `recording-isolation-core-1792507720348526234/core.log`, `recording-isolation-sync-1792507766098140559/sync.log`. Exact-baseline reproduction: `baseline-exact-fallback-1792507703326708814/canonical.log`; build recipe retained in `build-baseline.py` and generated backend/object/executable under `baseline/`.

The receiver/status baseline failure was not suppressed, its assertion was not weakened, and that unrelated implementation was not changed. The full gate remains failing; these results do not establish completion of this PRD.

### Next implementation seam

1. Finish the immutable request/ownership audit and measure owner preparation cost.
2. Add three bounded offscreen generations with separate producer/consumer completion and epoch/extent invalidation; keep the extracted synchronous lane as rollback.
3. Integrate current renderer output, original UI cadence and displayed-world interaction before exposing the mode as available. Prove original-UI R3/R6 and lifecycle R7/R8, then rerun all checks with current receiver changes integrated.

## 12. Historical checkpoint — completed-image backend

### Implemented

- `CompletedWorld.h` provides three bounded slots, one outstanding producer, latest-completed selection, separate producer/consumer completion timelines, stale-lease rejection and retained reader lifetimes. Busy requests are rejected rather than queued. Scene/extent invalidation cannot republish an old pending generation. Device/resize reset preserves monotonically distinct epoch/generation identities.
- `CGxDeviceVulkan::SceneCaptureCompletedWorld()` copies final default-target colour and depth, plus owner-supplied camera/viewport/scene metadata, into one retained generation. It rejects unfinished world/explicit-target scopes; allocation refusal preserves synchronous output and is logged on error changes rather than every frame.
- `SceneComposeCompletedWorld()` copies only the valid world rectangle into a current presentation, returning that image's metadata/lease together. It does not redraw world geometry or change the current logical camera. A new unfinished generation is never selected.
- Resize and device teardown destroy cache images only after the existing idle boundary. Explicit `SceneInvalidateCompletedWorld()` supports scene-owner lifecycle cancellation; the future `WorldFrame` integration must call it for loading/logout/map-owner changes.
- `CompletedWorldLiveTest.cpp` proves one rendered world image can survive 20 frames of changing UI draws without another world scope/render. Coverage includes raster, HDR, TAA and 50% scene scale, exact contained colour/alpha/depth, unchanged captured camera, deletion of original geometry, resize, missing first image and epoch invalidation.
- Actual terrain-RT acceptance now also captures and replays the shaded Goldshire frame and compares **all 307,200 colour/alpha/depth pixels exactly**, with the same supplied camera metadata. Existing caster-isolation and off/on/off assertions remain intact.

### Boundaries still not implemented

The cache is a production backend seam exercised by native GPU fixtures, not an enabled original-client scheduling mode. Capture/composition currently use the existing presentation recording lane and pass the same completed submission serial into both logical completion timelines. **This does not yet isolate producer and UI fences or provide asynchronous world production.** `CGWorldFrame::RenderWorld()` is unchanged; ordinary gameplay still renders a new world in each callback.

The camera/viewport publication is not an immutable actor/terrain/WMO interaction snapshot. Original picking, lifecycle handoff, owner capture cost, original-UI CPU-hold/input-latency acceptance and opt-in Video Settings remain pending. Do not expose this as a completed independent mode or claim R2/R3/R6/R7/R8 acceptance from these tests.

### Validation and retained evidence

Commands are the worktree-local build/CTest commands in section 7. Focused tests:

```sh
ctest --test-dir .wt/renderer-split/out/vulkan-tests --output-on-failure -R '^CompletedWorld$'
python3 .wt/renderer-split/out/checks/run-checks.py completed-world-modes \
  --pattern '^(CompletedWorld(Live|Hdr|Temporal|Scaled)Gpu|TerrainDirectionalSliceGpu)$' \
  --modes canonical core sync
```

- Compile-command-derived syntax diagnostics passed for the changed backend, new CPU/GPU fixtures and actual-world fixture. The standalone CPU slot test additionally passed strict `-Wall -Wextra -Werror` diagnostics. An optional strict-warning experiment on the live fixture rejected existing unused parameters in included Storm/Gx headers; no suppressions or unrelated header changes were added. Its evidence is retained in `completed-world-strict-header-warnings.log`.
- Native Vulkan and existing Vulkan+DXVK `Wow` builds passed. The complete Vulkan fixture build passed. Portability build/CTest passed **57/57**.
- Five targeted GPU cases passed in canonical, core and synchronization validation: `completed-world-modes-1792515657010076413/`.
- The final full suite, including the added resize checks, passed **50/51 in each of canonical/core/sync**. The only failure remains the exact-baseline-reproduced `TerrainDirectionalFallbackGpu` (`RT-off did not restore disabled status`). There were **zero Vulkan validation warnings/errors and zero skipped tests**. Core and sync each recorded 40 validation-layer instances and 41 devices across all fixtures.
- Full-suite evidence under `out/checks/`: `completed-world-verified-canonical-1792516249430629945/canonical.log`, `completed-world-verified-core-1792516430885555661/core.log`, and `completed-world-verified-sync-1792516478116604286/sync.log`. Build, syntax and portability logs use the `completed-world-` prefix. The full gate remains failing; the baseline failure was neither excluded nor weakened.

### Next implementation seam

Integrate an immutable producer handoff and a genuinely separate producer recording/completion lane with this publication ledger; keep all mutable game/model/Lua ownership on its current owner. Then connect the original `WorldFrame` callback, displayed-frame interaction and lifecycle invalidation before enabling the user-facing mode. Shared-queue GPU contention and shortening GPU draws remain explicitly deferred as clarified by the user.

## 13. Current checkpoint — original-client integration

### Implemented

- `CGWorldFrame::RenderWorld()` now schedules production, publishes completed output and reuses it through the original frame/strata render callback. The saved, default-off `nativeIndependentWorld` CVar is exposed by **Video Options → World Presentation → Update UI without a new 3D frame**, with Apply/Cancel/Defaults, support/status reporting and synchronous rollback.
- There is one Vulkan device and the existing swapchain. Mutable game, model, Lua, window and command-recording work stays on the owner thread. A bounded worker receives only a finished Vulkan command buffer and its fence; it submits after the UI prefix, using the shared queue mutex. There is at most one unfinished producer and three output slots. This is not a second renderer or a general render-command protocol.
- Producer command/descriptor/upload/framebuffer resources and completion are independent of UI resources. Source resident buffers, images, samplers and HDR LUTs used by the producer are pinned until completion. Mutated inputs receive new versions; UI-only retired resources can still be reclaimed at the UI fence. World timestamps, UI timestamps and legacy RT timestamps have explicit reset/lifetime handling, including scheduling changes.
- Completed generations carry final colour/depth, actual viewport/extent, camera transforms, HDR/TAA/scale metadata and matching immutable actor/M2/terrain/WMO/liquid interaction snapshots. Lighting status follows the displayed slot. TAA history uses world-generation identities rather than UI-frame counters.
- Original ray, screen-coordinate, closest-model, hit-test and default-action paths use displayed interaction data. Current owner eligibility is checked separately. Map/loading, camera-target/model-owner, frame-size, destruction and device-version changes withdraw stale interaction immediately. Aborted command streams are finalized/cancelled without publishing a stale generation.

### Validation

- Native Vulkan and Vulkan+DXVK `Wow` builds pass. Complete Vulkan-fixture build passes. Portability CTest passes **57/57**; snapshot and Video Options tests pass **4/4**.
- Six independent-mode GPU cases pass in canonical/core/sync, with **zero Vulkan diagnostics and zero skipped tests**: raster, HDR, TAA, scaled producer fixtures, plus original `WorldFrame`/strata/input fixtures with RT requested off/on. Producer tests mutate and delete source geometry/textures while held, check temporal history across UI-only gaps, and destroy the device with a held producer.
- The original-client fixtures use an offline real Goldshire world, original `CSimpleTop` update/render and mouse dispatch, changing native font UI, 1280×720, VSync/frame caps off, and twenty 500ms worker holds totalling ten seconds. They check frozen displayed-camera rays, generation pairing, immediate epoch withdrawal, recovery and synchronous rollback. The offline fixture defers the periodic server UI-timer request beyond its deadline; it does not stub or bypass production rendering.
- Latest full suite is **59/60 in canonical, core and sync**. The only failure is the previously exact-baseline-reproduced `TerrainDirectionalFallbackGpu`: `RT-off did not restore disabled status`. It remains enabled and unchanged. There are no Vulkan warnings/errors or skips. **The full gate is still failing**, not waived.
- Full-suite evidence: `out/checks/independent-world-full-canonical-1792524945861113608/`, `independent-world-full-core-1792525018760057069/`, and `independent-world-full-sync-1792525105762351447/`. Focused clean evidence: `independent-ui-validation-1792524748995218915/`. Build and portability logs use `independent-ui-` prefixes.

Canonical controlled-hold measurements from the latest full run:

| RT requested | UI presents/s while held | Present interval p95 | Input-to-UI-update p95 |
| --- | ---: | ---: | ---: |
| Off | 990.35 | 1.102ms | 0.048ms |
| On | 899.03 | 1.639ms | 0.251ms |

These are **controlled CPU-hold measurements, not ordinary gameplay FPS or input-to-photon latency**. Core/sync controlled-hold runs also satisfy ≥30 presents/s and ≤50ms p95 thresholds.

### Remaining sign-off and limitations

- The original-client integration is implemented; this is no longer a backend-only cache checkpoint. However, **full R6/live-game performance acceptance is not established**. Owner-thread update/recording and snapshot capture are still synchronous. Measurements include cold RT preparation around 878ms and intermittent warm owner-recording spikes above 50ms (roughly 30–100+ms, plus approximately 8–11ms interaction capture). Worker-hold results exclude those pre-handoff stalls. Do not describe all world CPU work as asynchronous or promise hitch-free gameplay from these numbers.
- Shared-queue GPU contention remains, explicitly accepted by the user. GPU draw/submission shortening is deferred; independent completion does not imply GPU preemption.
- A live logged-in session with populated actors/nameplates, repeated real loading/map transitions and the full independent-mode VSync-on matrix remains to be manually/integration validated. Existing offline, snapshot, resize/cache and VSync policy tests are not substitutes for that coverage.
- Changes remain uncommitted in `.wt/renderer-split`; root and `.wt/renderer` receiver work are untouched.

Reproduction:

```sh
cmake --build .wt/renderer-split/out/client-vulkan --target Wow --parallel 4
cmake --build .wt/renderer-split/out/client-both --target Wow --parallel 4
cmake --build .wt/renderer-split/out/vulkan-tests --parallel 4
ctest --test-dir .wt/renderer-split/out/portability-tests --output-on-failure
python3 .wt/renderer-split/out/checks/run-checks.py independent-ui-validation \
  --pattern '^IndependentWorld.*Gpu$' --modes canonical core sync
# Run separately: the preserved runner stops at the known failure in each mode.
for mode in canonical core sync; do
  python3 .wt/renderer-split/out/checks/run-checks.py independent-world-full-$mode --modes "$mode"
done
```

## 14. World black-flash investigation and camera-owner fix

The user clarified that the **3D world** flashes black while the UI remains visible; it is not the textured HUD disappearing. The independent-mode on/off comparison has not yet been performed.

### Reproduced cause and minimal fix

`CGWorldFrame::SetCameraTarget()` unconditionally advanced the display epoch, even when setting the same camera owner again. That immediately made the completed world unavailable while its replacement was still pending. The new original-client regression reproduced this with `Reapplying unchanged camera owner invalidated completed world` (`world-flash-owner-repro-1792531034586009184/`).

Removed the redundant outer invalidation. `CGCamera::SetTarget()` already invalidates when the target GUID actually changes or is cleared. The regression now verifies both idempotent target reassignment retaining the displayed image and removal of a previous camera owner withdrawing it immediately. **This fixes a demonstrated black-frame cause; confirmation that it accounts for all flashing in the user's live session is still pending.** No sleeps, extra GPU waits, relaxed completion checks or quality changes were added.

### Coverage and evidence

- Real `CSimpleFrame`/`CSimpleTexture` batching now renders an actual `Interface\\Buttons\\WHITE8X8` asset with per-frame changing tint, not just a standalone font overlay.
- Testing-only deferred pixel probes are appended to the ordinary UI command submission and read after the next normal UI fence. They do not split or synchronously submit a frame. Checks verify the current UI colour, non-black world output and exact retained world pixel equality while a generation is unchanged.
- Added `IndependentWorldUiVsyncGpu` with RT requested, FIFO/VSync on and the live preview's 1428×2516 portrait extent; existing 1280×720 VSync-off raster/RT cases remain. Corrected the fixture's ray coordinates to convert NDC to DDC for non-landscape aspect ratios.
- Compile-command-derived syntax checks passed for `WorldFrame.cpp`, the testing Vulkan backend and `TerrainDistanceWorldTest.cpp`. Native and Vulkan+DXVK builds pass. Portability passes **57/57**.
- Seven independent-rendering GPU cases pass in canonical/core/sync with zero Vulkan diagnostics or skips: `world-flash-owner-fixed-1792531118181134716/`.
- Latest full suite: **60/61 in every mode**, with the same unchanged `TerrainDirectionalFallbackGpu` failure (`RT-off did not restore disabled status`), zero Vulkan diagnostics and zero skips. Evidence: `world-flash-full-canonical-1792531340458611397/`, `world-flash-full-core-1792531427278157115/`, `world-flash-full-sync-1792531528001936698/` under `out/checks/`. The full gate remains failing.
- Earlier textured/deferred/retention checks did **not** reproduce spontaneous corruption or an unfinished-image selection; do not present those passes as proof that every reported live flash is resolved. The preview binary was not replaced or restarted during this investigation.


