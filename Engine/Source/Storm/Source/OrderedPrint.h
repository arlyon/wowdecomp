#ifndef ENGINE_SOURCE_STORM_SOURCE_ORDEREDPRINT_H
#define ENGINE_SOURCE_STORM_SOURCE_ORDEREDPRINT_H

#if defined(_WIN32)
#define STORM_ORDERED_PRINT _snprintf
#else
#include <cstdint>
#include <cstdio>
#include <cstring>

// soprintf reserves its final terminator separately: capacity counts payload
// bytes, unlike snprintf's count. The supplied buffer has capacity + 1 bytes.
template <typename... Args>
int StormOrderedPrint(char* output, int capacity, const char* format, Args... args) {
    return std::snprintf(output, static_cast<size_t>(capacity) + 1, format, args...);
}

inline int StormOrderedPrint(char* output, int capacity, const char* format, int64_t value) {
    const char conversion = format[std::strlen(format) - 1];
    if (conversion == 'u' || conversion == 'o' || conversion == 'x' || conversion == 'X') {
        return std::snprintf(output, static_cast<size_t>(capacity) + 1, format, static_cast<unsigned long long>(value));
    }
    return std::snprintf(output, static_cast<size_t>(capacity) + 1, format, static_cast<long long>(value));
}
#define STORM_ORDERED_PRINT StormOrderedPrint
#endif

#endif
