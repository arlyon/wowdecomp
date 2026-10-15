#include <cerrno>
#include <cstdint>
#include <mutex>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    if (!info) { errno = EINVAL; return false; }
    *info = {};
#if defined(__i386__) || defined(__x86_64__)
    unsigned a, b, c, d;
    if (!__get_cpuid(0, &a, &b, &c, &d) || !a) return false;
    info->m_maxLeaf = a;
    info->m_vendor[0] = b;
    info->m_vendor[1] = d;
    info->m_vendor[2] = c;
    __cpuid(1, a, b, c, d);
    info->m_leaf1Ebx = b;
    info->m_leaf1Edx = d;
    if (info->m_maxLeaf >= 4) {
        __cpuid_count(4, 0, a, b, c, d);
        info->m_leaf4Eax = a;
    }
    info->m_maxExtLeaf = __get_cpuid_max(0x80000000, nullptr);
    if (info->m_maxExtLeaf >= 0x80000001) {
        __cpuid(0x80000001, a, b, c, d);
        info->m_ext1Ecx = c;
        info->m_ext1Edx = d;
    }
    if (info->m_maxExtLeaf >= 0x80000004) {
        __cpuid(0x80000004, a, b, c, d);
        info->m_ext4Eax = a;
    }
    if (info->m_maxExtLeaf >= 0x80000008) {
        __cpuid(0x80000008, a, b, c, d);
        info->m_ext8Ecx = c;
    }
    return true;
#else
    errno = ENOTSUP;
    return false;
#endif
}

uint32_t System_Hardware::GetProcessorCount() {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) return 0;
    if (static_cast<uint64_t>(count) > UINT32_MAX) { errno = EOVERFLOW; return 0; }
    return static_cast<uint32_t>(count);
}

uint32_t Blizzard::Hardware::GetProcessorFeatures() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        System_Hardware::InitProcessorFeatures();
        System_Hardware::s_initialized = true;
    });
    return System_Hardware::s_processorFeatures.m_features;
}
