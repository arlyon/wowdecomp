# Keep explicit directional-light provenance with its original lighting owner

Status: accepted by the user (Option 1).

Retain the additive `CM2Lighting::m_shadowLighting` representation and explicit
`AddWorldDirectionalLight()` admission introduced in `68e99d5a`. Capture world
sun/moon, ambient and unshadowed local contributions during the original lighting
evaluation: the legacy aggregate can combine directional sources and is not a
safe input to shadow wholesale. Keeping the values with their existing owner
makes initialization, copying and reset explicit without another light selection
pass, shader counterfactuals or an address-keyed sidecar.

An explicit frame/evaluation packet was considered but requires broader callback
and lifetime plumbing; an external sidecar was rejected because temporary lighting
objects, copying and address reuse make ownership fragile. The inline choice adds
128 bytes in the measured Linux 64-bit build (`CM2Lighting`: 232-byte legacy prefix
plus provenance, 360 bytes total). Its original member prefix remains unchanged,
but containing-object size and subsequent member offsets change. This is a
rebuildable source representation, not a promise to preserve historical binary
layouts; supported builds and size-dependent allocations must be validated.

The GPU contract remains separate and explicitly versioned: never upload this CPU
object verbatim or rely on its padding/bool layout. Conflicting world directions
cannot silently become one shadow ray; the forthcoming readiness/receiver path
must handle or reject that case. Existing aggregate-parity and source/lifetime
tests are groundwork, not production RT or full corpus acceptance.
