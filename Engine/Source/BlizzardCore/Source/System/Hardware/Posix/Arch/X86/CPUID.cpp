#include <cerrno>
#include <cpuid.h>
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    if (!info) { errno = EINVAL; return false; }
    *info = {};
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
}
