#ifndef ENGINE_SOURCE_GX_DETAILDOODADINSTANCE_H
#define ENGINE_SOURCE_GX_DETAILDOODADINSTANCE_H

// Vertex-input adaptation only. The selected Wrath vertex/pixel programs still
// own lighting, wind, fog, fading and alpha testing. Rows are unscaled so the
// operation order matches the legacy CPU expansion: rotate, scale, translate.
struct GxDetailDoodadInstance {
    float rows[3][4];
    float normalScale[4];
    // Raw BGRA vertex-fetch order; the existing shader adapter owns swizzling.
    float colour[4];
};
static_assert(sizeof(GxDetailDoodadInstance)==80,"Detail instance shader ABI");

#endif
