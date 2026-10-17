#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_TIMING_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_TIMING_H

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace GxVulkan {
inline double TimestampMilliseconds(uint64_t begin, uint64_t end, uint32_t validBits, float periodNanoseconds) {
    if (!validBits || validBits > 64 || !std::isfinite(periodNanoseconds) || periodNanoseconds <= 0)
        throw std::invalid_argument("Invalid Vulkan timestamp properties");
    const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t{1} << validBits) - 1;
    return static_cast<double>((end - begin) & mask) * periodNanoseconds / 1000000.0;
}
}

#endif
