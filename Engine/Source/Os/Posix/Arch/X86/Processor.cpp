#include <cerrno>
#include <cstring>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/ProcessorFeatures.h"

int32_t IOsGetProcessorFeatures(CpuIdInfo* info) {
    if (!info) {
        errno = EINVAL;
        return 0;
    }
    *info = {};
#if defined(__i386__) || defined(__x86_64__)
    uint32_t a, b, c, d;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return 0;
    info->m_maxStandard = a;
    const uint32_t vendor[] = {b, d, c};
    std::memcpy(info->m_vendor, vendor, sizeof(vendor));
    if (info->m_maxStandard >= 1) {
        __cpuid(1, a, b, c, d);
        info->m_standard1b = b;
        info->m_standard1d = d;
    }
    if (info->m_maxStandard >= 4) {
        __cpuid_count(4, 0, a, b, c, d);
        info->m_standard4a = a;
    }
    info->m_maxExtended = __get_cpuid_max(0x80000000, nullptr);
    if (info->m_maxExtended >= 0x80000001) {
        __cpuid(0x80000001, a, b, c, d);
        info->m_extended1c = c;
        info->m_extended1d = d;
    }
    if (info->m_maxExtended >= 0x80000004) {
        for (uint32_t i = 0; i < 3; ++i) {
            __cpuid(0x80000002 + i, a, b, c, d);
            const uint32_t brand[] = {a, b, c, d};
            std::memcpy(info->m_brand + 16 * i, brand, sizeof(brand));
        }
    }
    if (info->m_maxExtended >= 0x80000008) {
        __cpuid(0x80000008, a, b, c, d);
        info->m_extended8c = c;
    }
    return 1;
#else
    errno = ENOTSUP;
    return 0;
#endif
}
