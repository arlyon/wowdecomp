#include "WoW/Source/Native/Startup.h"
#include <cerrno>
#include <sys/resource.h>

bool NativeStartupEnsureStack() {
    // WinMain reserves 8 MiB for its fiber. Linux can provide that budget on
    // the original main thread without fibers or transferring SDL ownership.
    constexpr rlim_t stackSize = 0x800000;
    rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) != 0) {
        return false;
    }
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= stackSize) {
        return true;
    }
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < stackSize) {
        errno = ENOMEM;
        return false;
    }
    limit.rlim_cur = stackSize;
    return setrlimit(RLIMIT_STACK, &limit) == 0;
}
