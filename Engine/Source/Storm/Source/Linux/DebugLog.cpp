#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Storm/H/storm.h"

namespace {
void Write(const char* format, va_list args) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    flockfile(stderr);
    std::fputs(buffer, stderr);
    const size_t length = std::strlen(buffer);
    if (!length || buffer[length - 1] != '\n') std::fputc('\n', stderr);
    funlockfile(stderr);
}
}

void DebugLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Write(format, args);
    va_end(args);
}

void DebugLog(int32_t, const char* format, ...) {
    va_list args;
    va_start(args, format);
    Write(format, args);
    va_end(args);
}

void DebugValidate(const char* expression) {
    DebugLog("Validation failed: %s", expression);
}
