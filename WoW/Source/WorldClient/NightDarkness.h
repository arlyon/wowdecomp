#ifndef WOW_WORLDCLIENT_NIGHTDARKNESS_H
#define WOW_WORLDCLIENT_NIGHTDARKNESS_H
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include "Engine/Source/Tempest/CImVector.h"

namespace NightDarkness {
inline std::optional<float> Parse(const char* text) {
    if (!text) return std::nullopt;
    char* end=nullptr; errno=0;
    const float value=std::strtof(text,&end);
    if (end==text || errno==ERANGE || !std::isfinite(value)) return std::nullopt;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end || value<0 || value>100) return std::nullopt;
    return value;
}
inline float Smooth(float value) {
    value=std::clamp(value,0.f,1.f);
    return value*value*(3.f-2.f*value);
}
inline float AmbientScale(float dayProgression,float reduction,bool protectedLighting=false) {
    if (protectedLighting || !std::isfinite(dayProgression) || !std::isfinite(reduction) || reduction<=0) return 1.f;
    const float hour=(dayProgression-std::floor(dayProgression))*24.f;
    // Continuous across midnight, with soft dawn/dusk rather than a clock switch.
    const float night=hour<12.f ? 1.f-Smooth((hour-4.f)/2.f) : Smooth((hour-18.f)/4.f);
    return 1.f-night*std::clamp(reduction,0.f,100.f)*.01f;
}
inline CImVector Ambient(CImVector colour,float scale) {
    if (!std::isfinite(scale)) return colour;
    scale=std::clamp(scale,0.f,1.f);
    colour.r=static_cast<uint8_t>(std::lround(colour.r*scale));
    colour.g=static_cast<uint8_t>(std::lround(colour.g*scale));
    colour.b=static_cast<uint8_t>(std::lround(colour.b*scale));
    return colour; // Alpha is unrelated to ambient intensity.
}
}
#endif
