#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_DEBUG_LINUX_NATIVE_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_DEBUG_LINUX_NATIVE_H

#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace System_Debug {
namespace Native {

inline bool DebuggerPresent() {
    FILE* status = std::fopen("/proc/self/status", "r");
    if (!status) {
        return false;
    }
    char line[256];
    bool traced = false;
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long tracer = 0;
        if (std::sscanf(line, "TracerPid: %lu", &tracer) == 1) {
            traced = tracer != 0;
            break;
        }
    }
    std::fclose(status);
    return traced;
}

inline void CreateDirectory(const char* directory) {
    // Only the executable-relative Errors child is created, not a File/MPQ backend.
    ::mkdir(directory, 0777);
}

inline FILE* OpenAppend(const char* path) {
    return std::fopen(path, "a");
}

}
}

#endif
