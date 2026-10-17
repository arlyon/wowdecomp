#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESIDENTBUFFER_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RESIDENTBUFFER_H

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace GxVulkan {
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
};
Buffer CreateBuffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
void DestroyBuffer(VkDevice device, Buffer& buffer);

// One Gx buffer's persistent device-local snapshot. The renderer owns retirement:
// old snapshots and staging allocations survive until its submission fence.
class ResidentBuffer {
public:
    bool NeedsUpload(VkDeviceSize bytes) const;
    void Destroy(VkDevice device); // Caller has waited for all uses.
    void MarkDirty() { m_dirty = true; }
    VkBuffer Resolve(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
        VkCommandBuffer command, uint64_t frame, const void* data, VkDeviceSize bytes,
        std::vector<Buffer>& retired, uint64_t& uploadedBytes);
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
