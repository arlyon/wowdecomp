#include "ResidentBuffer.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace GxVulkan {
namespace {
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
VkDeviceSize AlignedSize(VkDeviceSize bytes) {
    if (!bytes || bytes > std::numeric_limits<VkDeviceSize>::max() - 3)
        throw std::invalid_argument("Invalid resident buffer size");
    return (bytes + 3) & ~VkDeviceSize(3);
}
}
Buffer CreateBuffer(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        uint32_t type = memory.memoryTypeCount;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & properties) == properties) { type = i; break; }
        }
        if (type == memory.memoryTypeCount) throw std::runtime_error("Vulkan buffer has no suitable memory type");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory(buffer)");
        Check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
        if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            Check(vkMapMemory(device, buffer.memory, 0, size, 0, reinterpret_cast<void**>(&buffer.mapped)), "vkMapMemory(buffer)");
    } catch (...) { DestroyBuffer(device, buffer); throw; }
    return buffer;
}
void DestroyBuffer(VkDevice device, Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}
bool ResidentBuffer::NeedsUpload(VkDeviceSize bytes) const {
    return m_dirty || !m_buffer.buffer || m_sourceBytes != bytes;
}
void ResidentBuffer::Destroy(VkDevice device) {
    DestroyBuffer(device, m_buffer);
    m_dirty = true;
}
void ResidentBuffer::Retire(std::vector<Buffer>& retired) {
    if (m_buffer.buffer) { retired.push_back(m_buffer); m_buffer = {}; }
    m_dirty = true;
}
VkBuffer ResidentBuffer::Resolve(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory,
    VkCommandBuffer command, uint64_t frame, const void* data, VkDeviceSize bytes,
    std::vector<Buffer>& retired, uint64_t& uploadedBytes) {
    const auto size = AlignedSize(bytes);
    if (!data || !frame) throw std::invalid_argument("Invalid resident buffer source/frame");
    if (NeedsUpload(bytes)) {
        // Never overwrite a snapshot already referenced by commands in this
        // frame. Older frames have completed before the renderer calls Resolve.
        const bool replace = !m_buffer.buffer || m_buffer.size != size || m_usedFrame == frame;
        Buffer destination;
        Buffer staging;
        try {
            staging = CreateBuffer(device, memory, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));
            if (size != bytes) std::memset(staging.mapped + bytes, 0, static_cast<size_t>(size - bytes));
            if (replace) destination = CreateBuffer(device, memory, size,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            // Reserve ownership records before recording any commands; an
            // allocation failure must not free memory referenced by the queue.
            retired.reserve(retired.size() + (replace && m_buffer.buffer ? 2 : 1));
        } catch (...) {
            DestroyBuffer(device, staging); DestroyBuffer(device, destination); throw;
        }
        if (replace) { Retire(retired); m_buffer = destination; }
        retired.push_back(staging);
        VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(command, staging.buffer, m_buffer.buffer, 1, &copy);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = m_buffer.buffer; barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 0, nullptr, 1, &barrier, 0, nullptr);
        uploadedBytes += size;
        m_sourceBytes = bytes;
        m_dirty = false;
    }
    m_usedFrame = frame;
    return m_buffer.buffer;
}
}
