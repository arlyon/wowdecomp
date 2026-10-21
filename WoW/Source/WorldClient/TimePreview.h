#ifndef WOW_WORLDCLIENT_TIMEPREVIEW_H
#define WOW_WORLDCLIENT_TIMEPREVIEW_H
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <optional>

// Visual clock only. -1 preserves the caller's live/authored time; [0,24]
// freezes the visual hour, with 24 an alias for midnight (0). Fractions are valid.
namespace TimePreview {
inline std::optional<float> Parse(const char* text) {
    if (!text) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    float hour = std::strtof(text, &end);
    if (end == text || errno == ERANGE || !std::isfinite(hour)) return std::nullopt;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end || (hour != -1.f && (hour < 0.f || hour > 24.f))) return std::nullopt;
    return hour == 24.f ? 0.f : hour;
}
inline bool Apply(float hour, float& dayProgression, uint32_t& minutes) {
    if (!std::isfinite(hour) || hour < 0.f || hour > 24.f) return false;
    hour = hour == 24.f ? 0.f : hour;
    dayProgression = hour / 24.f;
    minutes = static_cast<uint32_t>(hour * 60.f);
    return true;
}
}
#endif
