#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <pwd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"

namespace {
std::atomic<int32_t> sleepInBackground{1};
std::atomic<uint32_t> backgroundSleepMs{0};
std::atomic<bool> cpuLogEnabled{false};

int32_t CopyName(const char* name, char* buffer, uint32_t* size, bool countNull) {
    const size_t length = std::strlen(name);
    const uint32_t required = static_cast<uint32_t>(length + 1);
    if (!buffer || *size < required) {
        if (buffer && *size) buffer[0] = '\0';
        *size = required;
        errno = ERANGE;
        return 0;
    }
    std::memcpy(buffer, name, required);
    // GetComputerName excludes NUL on success; GetUserName includes it.
    *size = static_cast<uint32_t>(length) + (countNull ? 1 : 0);
    return 1;
}
}

uint32_t OsGetNumberOfProcessors() {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<uint32_t>(count) : 1;
}

uint64_t OsGetPhysicalMemory() {
    struct sysinfo info{};
    if (sysinfo(&info) != 0) return 0;
    return static_cast<uint64_t>(info.totalram) * info.mem_unit;
}

uint32_t OsGetProcessorId() {
#if defined(__i386__) || defined(__x86_64__)
    return 0x783836; // Legacy x86 identity, not a CPUID signature.
#else
    return 0; // No legacy protocol identity for this architecture.
#endif
}

uint32_t OsGetOsId() {
    return 0x57696E; // Legacy authentication protocol tag 'Win'; system reporting remains Linux.
}

OsType OsGetVersion() {
    return OsType_Linux;
}

void OsGetVersionString(char* buffer, int32_t size) {
    if (!buffer || size <= 0) return;
    struct utsname info{};
    if (uname(&info) == 0) {
        std::snprintf(buffer, static_cast<size_t>(size), "%s %s %s", info.sysname, info.release, info.machine);
    } else {
        std::snprintf(buffer, static_cast<size_t>(size), "Linux");
    }
}

int32_t OsGetComputerName(char* buffer, uint32_t* size) {
    if (!size) {
        errno = EINVAL;
        return 0;
    }
    char name[HOST_NAME_MAX + 1]{};
    if (gethostname(name, sizeof(name)) != 0) {
        if (buffer && *size) buffer[0] = '\0';
        return 0;
    }
    name[HOST_NAME_MAX] = '\0';
    return CopyName(name, buffer, size, false);
}

int32_t OsGetUserName(char* buffer, uint32_t* size) {
    if (!size) {
        errno = EINVAL;
        return 0;
    }
    if (buffer && *size) buffer[0] = '\0';
    std::vector<char> storage(1024);
    struct passwd entry{};
    struct passwd* result = nullptr;
    int error;
    while ((error = getpwuid_r(geteuid(), &entry, storage.data(), storage.size(), &result)) == ERANGE) {
        storage.resize(storage.size() * 2);
    }
    if (error || !result || !entry.pw_name) {
        errno = error ? error : ENOENT;
        return 0;
    }
    return CopyName(entry.pw_name, buffer, size, true);
}

int32_t OsSleepInBackground() { return sleepInBackground.load(); }
void OsSetSleepInBackground(int32_t sleep) { sleepInBackground.store(sleep); }
uint32_t OsGetBackgroundSleepMs() { return backgroundSleepMs.load(); }
void OsSetBackgroundSleepMs(uint32_t milliseconds) { backgroundSleepMs.store(milliseconds); }

int32_t OsIsRemoteSession() {
    // Positive remote-login hints only: this does not detect compositor/display
    // remoting, and DISPLAY alone says nothing about whether a session is remote.
    const char* connection = std::getenv("SSH_CONNECTION");
    const char* tty = std::getenv("SSH_TTY");
    return (connection && *connection) || (tty && *tty);
}

void OsSystemEnableCpuLog() { cpuLogEnabled.store(true); }

void IOsSystemCpuLog(const CpuIdInfo* info) {
    if (!info || !cpuLogEnabled.exchange(false)) return;
    int32_t vendor;
    const uint32_t features = OsGetProcessorFeaturesEx(vendor);
    // CpuIdInfo's vendor and brand arrays are fixed-width, not NUL-terminated.
    std::fprintf(stderr, "CPU: vendor=%d features=%08x processors=%u vendor-id=%.12s brand=%.48s\n",
        vendor, features, OsGetNumberOfProcessors(), info->m_vendor, info->m_brand);
}

uint64_t OsGetProcessorTicksPerSecond() {
    if (cpuLogEnabled.load()) {
        CpuIdInfo info{};
        IOsGetProcessorFeatures(&info);
        IOsSystemCpuLog(&info);
    }
    // The sole client consumer is ConsoleDetect's nominal-MHz hardware tier.
    // CPUID.16H is base CPU frequency metadata, NOT a TSC or monotonic timer
    // conversion factor. Native timers retain OsGetAsyncClocksPerSecond().
#if defined(__i386__) || defined(__x86_64__)
    uint32_t a, b, c, d;
    if (__get_cpuid_max(0, nullptr) >= 0x16) {
        __cpuid(0x16, a, b, c, d);
        return static_cast<uint64_t>(a & 0xFFFF) * 1000000;
    }
#endif
    return 0; // Unknown, not a guessed GHz value or the timer's ns frequency.
}
