#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESOURCECACHE_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESOURCECACHE_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <variant>
#include "ResidentBuffer.h"

namespace GxVulkan {
struct ImageDescription {
    uint32_t width, height, levels, layers;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkDeviceSize allocationBytes = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool Matches(const ImageDescription& description) const;
};
void DestroyImage(VkDevice device, Image& image);

struct ResourceCacheStats {
    uint64_t imageAllocations = 0, imageReuses = 0, bufferAllocations = 0, bufferReuses = 0, evictions = 0;
};
// Owns ONLY idle allocations. Retired resources stay with the caller until
// Collect verifies their submission fence, which must cover every use in the
// supplied retirement lists (not a stale fence from an earlier submission).
// The budget counts Vulkan allocation requirements, not texel estimates;
// live/in-flight storage is not cache storage.
class ResourceCache {
public:
    static constexpr VkDeviceSize DefaultBudget = 32 * 1024 * 1024;
    static constexpr std::size_t EntryLimit = 128;
    static constexpr VkBufferUsageFlags ResidentUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ResourceCache(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
        VkDeviceSize budget = DefaultBudget);
    ~ResourceCache();
    ResourceCache(const ResourceCache&) = delete;
    ResourceCache& operator=(const ResourceCache&) = delete;
    Image AcquireImage(const ImageDescription& description);
    Buffer AcquireBuffer(VkDeviceSize size, bool& reused);
    bool Collect(VkFence completion, std::vector<Image>& images, std::vector<Buffer>& buffers);
    bool Collect(VkFence completion, std::vector<Image>& images) {
        std::vector<Buffer> empty;
        return Collect(completion, images, empty);
    }
    VkDeviceSize CachedBytes() const { return m_cachedBytes; }
    const ResourceCacheStats& Stats() const { return m_stats; }
    void ResetStats() { m_stats = {}; }
private:
    void Recycle(Image& image);
    void Recycle(Buffer& buffer);
    void MakeRoom(VkDeviceSize bytes);
    void EvictOldest();
    using Entry = std::variant<Image, Buffer>;
    void Destroy(Entry& entry);
    VkDevice m_device;
    VkPhysicalDeviceMemoryProperties m_memory;
    VkDeviceSize m_budget, m_cachedBytes = 0;
    std::vector<Entry> m_resources;
    ResourceCacheStats m_stats;
};
}
#endif
