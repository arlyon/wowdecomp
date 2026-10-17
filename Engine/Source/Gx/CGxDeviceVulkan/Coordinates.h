#ifndef ENGINE_GX_VULKAN_COORDINATES_H
#define ENGINE_GX_VULKAN_COORDINATES_H

#include <algorithm>
#include <vulkan/vulkan.h>

namespace GxVulkan {
// Gx screen rectangles are bottom-origin, but texture subrectangles are
// top-origin (matching D3D9). In both cases negative height maps +Y upward
// within the viewport. This matters for FFX's partially used POT textures.
inline VkViewport MakeViewport(VkExtent2D extent, bool textureTarget,
    float minX, float maxX, float minY, float maxY, float minDepth, float maxDepth) {
    return {minX * extent.width, (textureTarget ? maxY : 1.0f - minY) * extent.height,
        (maxX - minX) * extent.width, -(maxY - minY) * extent.height, minDepth, maxDepth};
}

inline VkRect2D MakeScissor(VkExtent2D extent, bool textureTarget,
    float minX, float maxX, float minY, float maxY) {
    const int left = std::clamp(static_cast<int>(minX * extent.width + 0.5f), 0, static_cast<int>(extent.width));
    const int top = std::clamp(static_cast<int>((textureTarget ? minY : 1.0f - maxY) * extent.height + 0.5f), 0, static_cast<int>(extent.height));
    const int right = std::clamp(static_cast<int>(maxX * extent.width + 0.5f), left, static_cast<int>(extent.width));
    const int bottom = std::clamp(static_cast<int>((textureTarget ? maxY : 1.0f - minY) * extent.height + 0.5f), top, static_cast<int>(extent.height));
    return {{left, top}, {static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)}};
}
}
#endif
