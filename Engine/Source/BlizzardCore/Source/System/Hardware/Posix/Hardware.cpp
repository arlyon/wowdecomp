#include <cerrno>
#include <cstdint>
#include <mutex>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

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
