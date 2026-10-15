#ifndef ENGINE_SOURCE_OS_LINUX_PROCESSOR_H
#define ENGINE_SOURCE_OS_LINUX_PROCESSOR_H

#include <cstdint>
struct CpuIdInfo;

struct OsLinuxProcessorFeatures {
    uint32_t features;
    int32_t vendor;
};

// Decode the legacy client bitmask separately from the hardware query so every
// vendor and optional-feature combination can be checked without CPU emulation.
OsLinuxProcessorFeatures OsLinuxDecodeProcessorFeatures(const CpuIdInfo* info);

#endif
