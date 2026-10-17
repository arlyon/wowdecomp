#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESIDENTBUFFER_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESIDENTBUFFER_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace GxVulkan {
class ResourceCache;
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0, allocationBytes = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags properties = 0;
};
Buffer CreateBuffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
void DestroyBuffer(VkDevice device, Buffer& buffer);

struct UploadStats {
    uint64_t stagingAllocations = 0, deviceAllocations = 0, stagingReuses = 0, deviceReuses = 0;
};
// Borrowed coherent staging storage. Its range must remain unchanged until the
// submission fence completes; the resident buffer never owns or retires it.
struct StagingSlice {
    const Buffer* buffer = nullptr;
    VkDeviceSize offset = 0;
};

// One Gx buffer's persistent device-local snapshot. The renderer owns retirement:
// old snapshots and staging allocations survive until its submission fence.
class ResidentBuffer {
public:
    bool NeedsUpload(VkDeviceSize bytes) const;
    void Destroy(VkDevice device); // Caller has waited for all uses.
    void MarkDirty() { m_dirty = true; }
    VkBuffer Resolve(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
        VkCommandBuffer command, uint64_t frame, const void* data, VkDeviceSize bytes,
        std::vector<Buffer>& retired, uint64_t& uploadedBytes,
        StagingSlice staging = {}, UploadStats* stats = nullptr, ResourceCache* cache = nullptr);
    void Retire(std::vector<Buffer>& retired);
    VkDeviceSize Size() const { return m_buffer.size; }
private:
    Buffer m_buffer;
    uint64_t m_usedFrame = 0;
    VkDeviceSize m_sourceBytes = 0;
    bool m_dirty = true;
};
}
#endif
