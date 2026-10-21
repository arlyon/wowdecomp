#ifndef ENGINE_SOURCE_OS_POSIX_WINDOWPLACEMENT_H
#define ENGINE_SOURCE_OS_POSIX_WINDOWPLACEMENT_H

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Window-manager units, never drawable pixels or a scene-resolution preference.
enum class OsWindowMode { Normal, WindowedFullscreen, ExclusiveFullscreen };
struct OsWindowPlacement {
    int x = 0, y = 0, width = 1024, height = 768;
    bool maximized = false;
};
inline OsWindowPlacement OsParseWindowPlacement(const char* value) {
    OsWindowPlacement result;
    if (!value) return result;
    const char* end = value + std::strlen(value);
    int fields[5];
    for (int index = 0; index < 5; ++index) {
        const auto parsed = std::from_chars(value, end, fields[index]);
        if (parsed.ec != std::errc{}) return result;
        value = parsed.ptr;
        if (index != 4) {
            if (value == end || *value++ != ',') return result;
        } else if (value != end) return result;
    }
    const auto x = fields[0], y = fields[1], width = fields[2], height = fields[3], maximized = fields[4];
    if (x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767
        && width >= 320 && width <= 16384 && height >= 240 && height <= 16384
        && (maximized == 0 || maximized == 1)) result = {x, y, width, height, maximized != 0};
    return result;
}
struct OsMonitorBounds { int x, y, width, height; };
inline size_t OsSelectWindowMonitor(const OsWindowPlacement& placement, const OsMonitorBounds* monitors, size_t count) {
    size_t selected = 0;
    int64_t largest = 0;
    for (size_t index = 0; index < count; ++index) {
        const auto& monitor = monitors[index];
        const int64_t width = std::max<int64_t>(0, std::min<int64_t>(int64_t(placement.x) + placement.width,
            int64_t(monitor.x) + monitor.width) - std::max(placement.x, monitor.x));
        const int64_t height = std::max<int64_t>(0, std::min<int64_t>(int64_t(placement.y) + placement.height,
            int64_t(monitor.y) + monitor.height) - std::max(placement.y, monitor.y));
        if (width * height > largest) { largest = width * height; selected = index; }
    }
    return selected; // No intersection: safe primary-monitor fallback.
}
inline OsWindowPlacement OsBoundWindowPlacement(OsWindowPlacement value, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return {};
    value.width = std::min(value.width, width);
    value.height = std::min(value.height, height);
    value.x = std::clamp(value.x, x, x + width - value.width);
    value.y = std::clamp(value.y, y, y + height - value.height);
    return value;
}
#endif
