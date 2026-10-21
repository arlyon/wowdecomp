# Inline directional visibility on one shared acceleration scene

Use inline fragment ray queries for the first directional-shadow milestone, with
one shared acceleration scene and shared authored coverage evaluation. A bounded
Radeon 890M experiment compared inline queries against a receiver-buffer
raster/compute/composite path: both matched each other and an independent CPU
oracle for 24 frames × 3,072 samples in canonical/core/sync validation, including
non-vacuous posed/alpha/UV/threshold/light changes, terrain holes and a transformed
building proxy. Inline avoids the extra receiver passes and 110,592 bytes of
receiver storage in that 96×32 experiment; its smaller ownership footprint is the
primary reason for choosing it, not an extrapolated performance claim.

The production ordering also has a narrow preparation seam: `CWorldScene::Render`
advances/animates the original M2 scene before terrain/WMO draws. The later unit
`PostAnimate` hook only updates nameplates. Shared caster preparation can therefore
use current original poses before the first receiver draw without adding a second
world renderer or moving gameplay animation. Deferred/off-camera instances still
require explicit shared initialization/current-frame guards and dedicated tests.

The private reproducible probe and source/SPIR-V fingerprints are recorded in
`out/renderer-reset/rt-feasibility/decision.json` at the workspace root. It is a
synthetic feasibility result, not loaded-world, material-corpus, attachment,
residency, production budget or final shadow acceptance. Production work must
reuse original loaded geometry/current poses and authored material decisions;
opaque casting must not wait for diffuse/emission textures. An explicit shader
interface extension will expose shared visibility/coverage resources without
allocating unused GI, sky-capture or fog buffers. Maintained source applies
visibility only to its directional contribution; generated compatibility code is
not an RT receiver and cannot be counted as complete coverage.
