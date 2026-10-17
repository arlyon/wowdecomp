#include <cmath>
#include <algorithm>
#include "Engine/Source/Base/Coordinate.h"

// STATIC: 0x00AC0CB0
static float s_aspectRatio = 4.0f / 3.0f;

// STATIC: 0x00AC0CB4
static float s_ddcWidthScale = 0.8f;

// STATIC: 0x00AC0CB8
static float s_ddcHeightScale = 0.6f;

// STATIC: 0x00AC0CBC
static float s_aspectCompensation = 1.0f;

float CoordinateWindowAspect(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
        return 4.0f / 3.0f;
    const float aspect = width / height;
    return std::isfinite(aspect) && aspect > 0.0f ? aspect : 4.0f / 3.0f;
}

float CoordinateWindowUiScale(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f)
        return 1.0f;
    // Fit the classic 1024x768 layout uniformly; narrow tiles need to be able
    // to go below the original fixed UI-scale floor instead of stretching.
    return std::min({1.0f, 768.0f / height, CoordinateWindowAspect(width, height) * 0.75f});
}

// VERIFY: a3f58d50 manual
// OFFSET: 0x0047BF90
void CoordinateSetAspectRatio(float aspect) {
#if !defined(_WIN32) || defined(WOWDECOMP_VULKAN)
    aspect = CoordinateWindowAspect(aspect, 1.0f);
#endif
    s_aspectRatio = aspect;
    s_aspectCompensation = aspect * 0.75f;
#if !defined(_WIN32) || defined(WOWDECOMP_VULKAN)
    // Keep logical UI distances stable as the window changes aspect. The old
    // diagonal normalisation changed every stored width/offset/font size and
    // required recreating FrameXML to re-express those distances after resize.
    s_ddcHeightScale = 0.6f;
    s_ddcWidthScale = aspect * s_ddcHeightScale;
#else
    float scale = 1.0f / sqrt(aspect * aspect + 1.0f);
    s_ddcHeightScale = scale;
    s_ddcWidthScale = aspect * scale;
#endif
}

// VERIFY: 61e66992 auto
// OFFSET: 0x0047BFD0
float CoordinateGetAspectRatio() {
    return s_aspectRatio;
}

// VERIFY: 33d3d8ec auto
// OFFSET: 0x0047BFE0
float CoordinateGetAspectCompensation() {
    return s_aspectCompensation;
}

// VERIFY: 219e50ed auto
// OFFSET: 0x0047BFF0
void NDCToDDC(float ndcx, float ndcy, float* ddcx, float* ddcy) {
    if (ddcx) {
        *ddcx = s_ddcWidthScale * ndcx;
    }

    if (ddcy) {
        *ddcy = s_ddcHeightScale * ndcy;
    }
}

// VERIFY: 55a28a00 auto
// OFFSET: 0x0047C020
void DDCToNDC(float ddcx, float ddcy, float* ndcx, float* ndcy) {
    if (ndcx) {
        *ndcx = ddcx / s_ddcWidthScale;
    }

    if (ndcy) {
        *ndcy = ddcy / s_ddcHeightScale;
    }
}

// VERIFY: 7b40f308 auto
// OFFSET: 0x0047C050
float DDCToNDCWidth(float ddcx) {
    return ddcx / s_ddcWidthScale;
}

// VERIFY: aa52e55e auto
// OFFSET: 0x0047C060
float DDCToNDCHeight(float ddcy) {
    return ddcy / s_ddcHeightScale;
}

// VERIFY: e46d6d9e auto
// OFFSET: 0x0047C070
float NDCToDDCWidth(float ndcx) {
    return s_ddcWidthScale * ndcx;
}

// VERIFY: 65cc685c auto
// OFFSET: 0x0047C080
float NDCToDDCHeight(float ndcy) {
    return s_ddcHeightScale * ndcy;
}
