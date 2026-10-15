#include <cerrno>
#include <cstring>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/Linux/Processor.h"

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

OsLinuxProcessorFeatures OsLinuxDecodeProcessorFeatures(const CpuIdInfo* info) {
    OsLinuxProcessorFeatures result{0x80000000, 4};
    if (!info) return result;
    result.vendor = 0;
    if (info->m_standard1d & 0x10) result.features |= 1;
    if (info->m_standard1d & 0x800000) result.features |= 2;
    if (info->m_standard1d & 0x2000000) result.features |= 4;
    if (info->m_extended1d & 0x80000000) result.features |= 8;
    if (info->m_standard1d & 0x4000000) result.features |= 0x10;
    if (std::memcmp(info->m_vendor, "GenuineIntel", 12) == 0) {
        result.vendor = 1;
        if (info->m_maxStandard >= 4 && (info->m_standard4a >> 26)) result.features |= 0x40;
    } else if (std::memcmp(info->m_vendor, "AuthenticAMD", 12) == 0) {
        result.vendor = 2;
        if (static_cast<uint8_t>(info->m_extended8c)) result.features |= 0x40;
    }
    return result;
}

uint32_t OsGetProcessorFeaturesEx(int32_t& vendor) {
    // Function-local initialization publishes the whole result atomically to
    // concurrent callers, rather than exposing a partially populated cache.
    static const OsLinuxProcessorFeatures cached = [] {
        CpuIdInfo info;
        return OsLinuxDecodeProcessorFeatures(IOsGetProcessorFeatures(&info) ? &info : nullptr);
    }();
    vendor = cached.vendor;
    return cached.features;
}
