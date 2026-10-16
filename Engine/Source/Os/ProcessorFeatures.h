#ifndef ENGINE_SOURCE_OS_PROCESSORFEATURES_H
#define ENGINE_SOURCE_OS_PROCESSORFEATURES_H

#include <cstdint>
struct CpuIdInfo;

struct OsProcessorFeatures {
    uint32_t features;
    int32_t vendor;
};

// Decode the legacy client bitmask separately from the hardware query so every
// vendor and optional-feature combination can be checked without CPU emulation.
OsProcessorFeatures OsDecodeProcessorFeatures(const CpuIdInfo* info);

#endif
