#include "WoW/Source/Client.h"
#include "WoW/Source/Linux/Startup.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/AccountData.h"
#include <cerrno>
#include <cfenv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

// Security scheduling inputs only; no fake CommonMain or scanner providers.
uint8_t s_botCheckEnabled;
int32_t s_botCheckDelay;
volatile long s_additionalCheckArmed;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckFloatingPoint() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int a, b, c, d;
    Check(__get_cpuid(1, &a, &b, &c, &d) && (d & bit_FXSAVE) &&
          (d & bit_SSE) && (d & bit_SSE2), "real hardware supports native FP controls");
    fenv_t saved;
    Check(std::fegetenv(&saved) == 0, "save FP environment");
    alignas(16) unsigned char fxsave[512]{};
    __asm__ volatile("fxsave %0" : "=m"(fxsave));
    unsigned int supported;
    std::memcpy(&supported, fxsave + 28, sizeof(supported));
    unsigned short original;
    __asm__ volatile("fnstcw %0" : "=m"(original));
    for (unsigned short precision : {0x0000, 0x0200, 0x0300}) {
        unsigned short dirty = (original & ~0x0F00u) | precision | 0x0C3Fu;
        __asm__ volatile("fldcw %0" : : "m"(dirty));
        unsigned int mxcsr = 0xFFBFu; // All flags/masks, chop, FTZ; optional DAZ below.
        if (supported & 0x40) mxcsr |= 0x40;
        __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
        Check(std::feraiseexcept(FE_ALL_EXCEPT) == 0, "raise masked pending exceptions");
        Check(NativeStartupInitializeFloatingPoint(), "initialize real FP state");
        unsigned short actual, status;
        __asm__ volatile("fnstcw %0\n\tfnstsw %1" : "=m"(actual), "=m"(status));
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        // MSVC float.h: EM=0x8001F (all six), RC_NEAR=0, DN_SAVE=0;
        // Win64's mask excludes PC/IC. Check the native registers, not fenv aliases.
        Check((actual & 0x003F) == 0x003F && (actual & 0x0C00) == 0, "x87 all masked and nearest");
        Check((actual & ~0x0C3Fu) == (dirty & ~0x0C3Fu), "x87 precision/infinity and unrelated bits preserved");
        Check((status & 0x00FF) == 0, "x87 pending exceptions cleared");
        Check((mxcsr & 0xFFFF) == 0x1F80, "SSE all masked, nearest, no pending flags, DAZ/FTZ off");
        volatile double denormal = 0x1p-1074;
        volatile double one = 1.0;
        Check(denormal * one == denormal, "denormal operand/result preserved");
    }
    Check(std::fesetenv(&saved) == 0, "restore FP environment");
#else
    errno = 0;
    Check(!NativeStartupInitializeFloatingPoint() && errno == ENOTSUP, "non-x86 is explicitly unsupported");
#endif
}

void CheckChild(void (*operation)(), int signal) {
    pid_t pid = fork();
    Check(pid >= 0, "fork isolated check");
    if (pid == 0) {
        rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        operation();
        _exit(0);
    }
    int status;
    Check(waitpid(pid, &status, 0) == pid, "wait for isolated check");
    Check(signal ? WIFSIGNALED(status) && WTERMSIG(status) == signal :
                   WIFEXITED(status) && WEXITSTATUS(status) == 0, "isolated check outcome");
}

void CheckStack() {
    rlimit before;
    Check(getrlimit(RLIMIT_STACK, &before) == 0, "get stack limit");
    if (before.rlim_max == RLIM_INFINITY || before.rlim_max >= 0x800000) {
        rlimit small{0x400000, before.rlim_max};
        Check(setrlimit(RLIMIT_STACK, &small) == 0, "lower soft stack limit");
        const long thread = syscall(SYS_gettid);
        Check(NativeStartupEnsureStack(), "raise main-thread stack budget");
        rlimit after;
        Check(getrlimit(RLIMIT_STACK, &after) == 0 && after.rlim_cur == 0x800000 &&
              after.rlim_max == before.rlim_max, "bounded soft-only stack change");
        Check(syscall(SYS_gettid) == thread && thread == getpid(), "original main thread retained");
    }
    rlimit small{0x400000, 0x400000};
    Check(setrlimit(RLIMIT_STACK, &small) == 0, "lower hard limit in child only");
    Check(!NativeStartupEnsureStack() && errno == ENOMEM, "insufficient hard stack budget fails");
}
}

int main() {
    CheckFloatingPoint();
    CheckChild(CheckStack, 0);
    s_botCheckEnabled = 0;
    s_botCheckDelay = 1;
    s_additionalCheckArmed = 1;
    AdditionalCheck1();
    Check(s_additionalCheckArmed == 1, "disabled check stays armed");
    s_botCheckEnabled = 1;
    for (int delay : {0, -1, 421}) {
        s_botCheckDelay = delay;
        AdditionalCheck1();
        Check(s_additionalCheckArmed == 1, "inactive delay stays armed");
    }
    s_botCheckDelay = 1;
    s_additionalCheckArmed = 0;
    AdditionalCheck1();
    s_additionalCheckArmed = 1;
    CheckChild([] { AdditionalCheck1(); }, SIGABRT);
    CheckChild([] { InitTokenSeed(nullptr); }, SIGABRT);
    CheckChild([] { Packet_SMSG_CHECK_FOR_BOTS(nullptr, SMSG_CHECK_FOR_BOTS, 0, nullptr); }, SIGABRT);
    CheckChild([] { GatherAdditionalChecks(nullptr, nullptr, nullptr); }, SIGABRT);
    CheckChild([] { AdditionalCheck1Scan(nullptr, 0); }, SIGABRT);
    std::puts("PASS: native FP/CPU controls, main-thread stack budget, fail-closed security gates; CommonMain not linked or run");
}
