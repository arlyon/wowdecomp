#include <cerrno>
#include <unistd.h>
#include "Engine/Source/Os/Path.h"

uint32_t GetModuleFileNameUTF8(void* module, char* path, uint32_t size) {
    if (!path || !size) {
        errno = EINVAL;
        return 0;
    }
    path[0] = '\0';
    // No callers request a shared-library handle; a dlopen handle is not a path.
    if (module) {
        errno = ENOTSUP;
        return 0;
    }
    const ssize_t length = readlink("/proc/self/exe", path, size);
    if (length < 0) return 0;
    if (static_cast<uint64_t>(length) >= size) {
        // Never expose a truncated executable name (or directory) as valid.
        path[0] = '\0';
        errno = ENAMETOOLONG;
        return 0;
    }
    path[length] = '\0';
    return static_cast<uint32_t>(length);
}
