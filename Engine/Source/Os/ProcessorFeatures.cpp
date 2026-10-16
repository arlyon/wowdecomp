#include "Engine/Source/Platform/Platform.h"
#if WOW_PLATFORM_POSIX
#include <cstring>
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/ProcessorFeatures.h"

OsProcessorFeatures OsDecodeProcessorFeatures(const CpuIdInfo* info) {
    OsProcessorFeatures result{0x80000000, 4};
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
    static const OsProcessorFeatures cached = [] {
        CpuIdInfo info;
        return OsDecodeProcessorFeatures(IOsGetProcessorFeatures(&info) ? &info : nullptr);
    }();
    vendor = cached.vendor;
    return cached.features;
}
#endif
