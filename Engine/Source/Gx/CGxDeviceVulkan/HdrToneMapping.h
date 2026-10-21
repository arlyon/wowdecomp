#ifndef GX_VULKAN_HDR_TONE_MAPPING_H
#define GX_VULKAN_HDR_TONE_MAPPING_H
#include <cmath>

namespace GxVulkan {
// Finite manual EV only. Production defaults are Off and EV0.
inline bool HdrExposureValid(float ev) { return std::isfinite(ev) && ev >= -8.f && ev <= 8.f; }

// Scene-linear transport: manual exposure, identity below a 0.5
// linear knee, C1-continuous asymptotic shoulder above it. No image statistics,
// normalization, ambient lift, or decoding of legacy lit/material output.
// Negative/nonfinite scene RGB maps to black; alpha is never an input here.
// Call only with validated EV. Reciprocal form avoids overflow for finite RGB.
inline float HdrToneMapLinear(float sceneLinear, float exposureEV) {
    if (!std::isfinite(sceneLinear) || sceneLinear <= 0.f)
        return 0.f;
    const float scale = std::exp2(exposureEV);
    return sceneLinear <= .5f / scale ? sceneLinear * scale : 1.f - .25f / sceneLinear / scale;
}
// SDR UNORM output stores explicitly encoded RGB (not an sRGB attachment).
inline float HdrEncodeSdr(float linear) {
    return linear <= .0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
}
} // namespace GxVulkan
#endif
