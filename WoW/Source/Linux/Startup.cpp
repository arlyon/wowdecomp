#include "WoW/Source/Linux/Startup.h"
#include <cerrno>
#include <sys/resource.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

bool NativeStartupInitializeFloatingPoint() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int a, b, c, d;
    // The native control path below requires FXSAVE/SSE/SSE2, not invented
    // feature bits. These are architectural requirements on x86-64.
    if (!__get_cpuid(1, &a, &b, &c, &d) ||
        (d & (bit_FXSAVE | bit_SSE | bit_SSE2)) != (bit_FXSAVE | bit_SSE | bit_SSE2)) {
        errno = ENOTSUP;
        return false;
    }

    // Win64 main: _clearfp(); _control87(..., _MCW_DN|_MCW_EM|_MCW_RC).
    // MSVC's 0x8001F masks ALL six exceptions; DN_SAVE and RC_NEAR are zero.
    // Native bit encodings differ. Preserve x87 precision/infinity controls.
    unsigned short control;
    __asm__ volatile("fnclex\n\tfnstcw %0" : "=m"(control));
    control = (control & ~0x0C00u) | 0x003Fu;
    __asm__ volatile("fldcw %0" : : "m"(control));

    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    // Clear pending flags, DAZ, rounding and FTZ; mask all SSE exceptions.
    mxcsr = (mxcsr & ~0xE07Fu) | 0x1F80u;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    return true;
#else
    errno = ENOTSUP;
    return false;
#endif
}

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
