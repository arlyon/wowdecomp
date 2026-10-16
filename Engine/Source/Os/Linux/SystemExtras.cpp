#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <link.h>
#include <SDL.h>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
struct ExecutableQuery {
    uintptr_t address;
    bool foundMain = false;
    bool executable = false;
};
int CheckMainExecutable(dl_phdr_info* info, size_t, void* argument) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    auto& query = *static_cast<ExecutableQuery*>(argument);
    query.foundMain = true;
    constexpr uintptr_t limit = std::numeric_limits<uintptr_t>::max();
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X)) continue;
        if (segment.p_vaddr > limit - info->dlpi_addr) continue;
        const uintptr_t start = info->dlpi_addr + segment.p_vaddr;
        if (query.address >= start && query.address - start < segment.p_memsz) {
            query.executable = true;
            break;
        }
    }
    return 1;
}
}

void OsValidateFunctionPointer(const void* ptr) {
    ExecutableQuery query{reinterpret_cast<uintptr_t>(ptr)};
    // Main executable PF_X load segments, including the real PIE load bias;
    // shared-library code and non-executable data are not accepted callbacks.
    dl_iterate_phdr(CheckMainExecutable, &query);
    if (ptr && query.executable) return;
    if (!query.foundMain) SErrDisplayAppFatal("Unable to find main executable text segments");
    else SErrDisplayAppFatal("Invalid function pointer: %p", ptr);
    // The fatal provider must terminate; never continue if a provider returns.
    std::abort();
}

int32_t OsLaunchURL(const char* url) {
    if (!url || !*url) { errno = EINVAL; return 0; }
    if (std::strlen(url) >= 1024) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        errno = EINVAL;
        return 0;
    }
    // SDL accepts a URI, not a ShellExecute filename. Preserve percent escapes
    // and never interpolate the URI into a shell command ourselves.
    return SDL_OpenURL(url) == 0;
}

void* CreateNamedEventA(const char*, int32_t) {
    // CommonMain discards this optional launcher-presence handle. An anonymous
    // event would not implement named Windows-compatible interprocess presence.
    errno = ENOTSUP;
    return nullptr;
}
