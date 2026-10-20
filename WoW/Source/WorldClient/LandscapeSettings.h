#ifndef WOW_SOURCE_WORLDCLIENT_LANDSCAPESETTINGS_H
#define WOW_SOURCE_WORLDCLIENT_LANDSCAPESETTINGS_H
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace LandscapeSettings {
constexpr float Minimum = 4000.f;
constexpr float Maximum = 10000.f;
constexpr float Step = 500.f;
constexpr int SliderMaximum = 13; // Classic, then 4000..10000.
inline std::optional<float> Parse(const char* text) {
    if (!text) return std::nullopt;
    char* end = nullptr; errno = 0;
    const float value = std::strtof(text,&end);
    if (end == text || errno == ERANGE || !std::isfinite(value)) return std::nullopt;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end || (value != 0 && (value < Minimum || value > Maximum))) return std::nullopt;
    return value;
}
inline float Horizon(float detailDistance,float classicScale,float setting) {
    return setting > 0 ? setting : detailDistance*classicScale;
}
inline float FogScale(float authoredEnd,float setting) {
    // The requested horizon is the outdoor fog endpoint, not a ratio to the
    // old horizon projection (which is several times farther than authored fog).
    return setting > 0 && authoredEnd > 0 ? setting/authoredEnd : 1.f;
}
inline float FromSlider(int index) {
    if (index <= 0) return 0;
    if (index > SliderMaximum) index = SliderMaximum;
    return Minimum+(index-1)*Step;
}
inline int ToSlider(float value) {
    if (value <= 0) return 0;
    int index = 1+static_cast<int>(std::lround((value-Minimum)/Step));
    return index < 1 ? 1 : index > SliderMaximum ? SliderMaximum : index;
}
}
#endif
