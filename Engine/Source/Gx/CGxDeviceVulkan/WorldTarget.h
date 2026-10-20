#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_WORLDTARGET_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_WORLDTARGET_H

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace GxVulkan {
// Gx viewports are bottom-up; Vulkan image copies use top-down pixel rectangles.
inline VkRect2D WorldViewportRect(VkExtent2D size, float left, float right, float bottom, float top) {
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(bottom) || !std::isfinite(top) ||
        right < left || top < bottom)
        throw std::invalid_argument("Invalid world viewport");
    if (size.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        size.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        throw std::invalid_argument("World viewport exceeds image offset range");
    if (!size.width || !size.height || right == left || top == bottom) return {};
    const auto pixel = [](float value, uint32_t limit, bool high) {
        const double scaled = static_cast<double>(std::clamp(value, 0.f, 1.f)) * limit;
        return static_cast<int32_t>(high ? std::ceil(scaled) : std::floor(scaled));
    };
    const int32_t x = pixel(left, size.width, false), y = pixel(1.f - top, size.height, false);
    const int32_t endX = pixel(right, size.width, true), endY = pixel(1.f - bottom, size.height, true);
    return {{x, y}, {static_cast<uint32_t>(endX - x), static_cast<uint32_t>(endY - y)}};
}

// Finite, bounded request; ceil keeps tiny output dimensions nonempty.
inline VkExtent2D WorldRenderExtent(VkExtent2D output, float scale, uint32_t limit) {
    if (!std::isfinite(scale) || scale < .5f || scale > 1.f ||
        !output.width || !output.height || output.width > limit || output.height > limit)
        throw std::invalid_argument("Invalid scene render extent");
    return {std::max(1u, uint32_t(std::ceil(double(output.width) * scale))),
        std::max(1u, uint32_t(std::ceil(double(output.height) * scale)))};
}

// Capability-gated by the caller, including depth BLIT_SRC/DST support. Nearest
// depth blits preserve device depth (never interpolate across silhouettes).
inline void BlitWorldImage(VkCommandBuffer command, VkImage source, VkImage destination,
    VkImageAspectFlags aspect, VkRect2D from, VkRect2D to, VkFilter filter = VK_FILTER_NEAREST) {
    if (!from.extent.width || !from.extent.height || !to.extent.width || !to.extent.height) return;
    const auto barrier = [&](VkImage image, VkImageLayout before, VkImageLayout after) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = before; b.newLayout = after;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {aspect,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,0,nullptr,0,nullptr,1,&b);
    };
    barrier(source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(destination,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {aspect,0,0,1};
    blit.srcOffsets[0] = {from.offset.x,from.offset.y,0};
    blit.srcOffsets[1] = {from.offset.x+int32_t(from.extent.width),from.offset.y+int32_t(from.extent.height),1};
    blit.dstOffsets[0] = {to.offset.x,to.offset.y,0};
    blit.dstOffsets[1] = {to.offset.x+int32_t(to.extent.width),to.offset.y+int32_t(to.extent.height),1};
    vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    barrier(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    barrier(destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
}

// Both images enter/leave in GENERAL, as required by the existing Gx target path.
// Identical formats/extents preserve colour/depth bits; this is not AA or upscaling.
inline void CopyWorldImage(VkCommandBuffer command, VkImage source, VkImage destination,
    VkImageAspectFlags aspect, VkRect2D rect) {
    if (!rect.extent.width || !rect.extent.height) return;
    const auto transition = [&](VkImage image, VkImageLayout before, VkImageLayout after,
        VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    };
    transition(source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    transition(destination, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {aspect, 0, 0, 1};
    copy.srcOffset = copy.dstOffset = {rect.offset.x, rect.offset.y, 0};
    copy.extent = {rect.extent.width, rect.extent.height, 1};
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    transition(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
}
#endif
