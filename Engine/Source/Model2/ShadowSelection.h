#ifndef MODEL2_SHADOW_SELECTION_H
#define MODEL2_SHADOW_SELECTION_H
#include "Engine/Source/Gx/ShadowCoverage.h"
#include <optional>

namespace M2Shadow {
// Original authored batch/material selection, shared by the legacy shadow
// collector and directional RT. No texture readiness or lighting flag enters
// this decision: opaque casters cannot be delayed by diffuse/emission loading.
inline std::optional<GxShadow::ExclusionReason> BatchExclusion(uint16_t shader,uint8_t flags,uint16_t materialLayer) {
    if(shader==0x8000 || (flags&4) || materialLayer)return GxShadow::ExclusionReason::BatchExcluded;
    return std::nullopt;
}
inline std::optional<GxShadow::ExclusionReason> MaterialExclusion(uint16_t flags,uint16_t blendMode) {
    if(flags&0x40)return GxShadow::ExclusionReason::MaterialNoCast;
    if(blendMode>1 && !(flags&0x80))return GxShadow::ExclusionReason::BlendNotForced;
    return std::nullopt;
}
inline GxShadow::Coverage MaterialCoverage(uint16_t flags,uint16_t blendMode,float opacity) {
    if(const auto reason=MaterialExclusion(flags,blendMode))return GxShadow::NonCaster{*reason};
    // Retain the original inclusive batch-fade boundary and NaN rejection.
    // This is the batch admission threshold, not a sampled texture alpha ref.
    if(!(opacity>=0.55000001f))return GxShadow::NonCaster{GxShadow::ExclusionReason::AnimatedAlpha};
    if(!blendMode)return GxShadow::Opaque{};
    if(blendMode==1)return GxShadow::Cutout{};
    return GxShadow::ForcedCutout{};
}
}
#endif
