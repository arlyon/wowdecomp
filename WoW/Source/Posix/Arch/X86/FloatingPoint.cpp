#include "WoW/Source/Posix/Startup.h"
#include <cerrno>
#include <cpuid.h>

bool NativeStartupInitializeFloatingPoint() {
    unsigned int a, b, c, d;
    // The native control path requires real FXSAVE/SSE/SSE2 support.
    if (!__get_cpuid(1, &a, &b, &c, &d) ||
        (d & (bit_FXSAVE | bit_SSE | bit_SSE2)) != (bit_FXSAVE | bit_SSE | bit_SSE2)) {
        errno = ENOTSUP;
        return false;
    }

    // Win64 main: _clearfp(); _control87(..., _MCW_DN|_MCW_EM|_MCW_RC).
    // Mask all six exceptions, preserve x87 precision/infinity controls.
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
}
