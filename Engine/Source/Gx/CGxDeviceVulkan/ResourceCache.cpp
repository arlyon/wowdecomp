#include "ResourceCache.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace GxVulkan {
namespace {
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
}
bool Image::Matches(const ImageDescription& d) const {
    return width == d.width && height == d.height && levels == d.levels && layers == d.layers &&
        format == d.format && aspect == d.aspect && usage == d.usage;
}
void DestroyImage(VkDevice device, Image& image) {
    if (image.sampler) vkDestroySampler(device, image.sampler, nullptr);
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}
ResourceCache::ResourceCache(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, VkDeviceSize budget)
    : m_device(device), m_memory(memory), m_budget(budget) {
    if (!device) throw std::invalid_argument("Resource cache requires a Vulkan device");
    // No host allocation is needed while consuming completed retirement lists.
    m_resources.reserve(EntryLimit);
}
ResourceCache::~ResourceCache() {
    for (auto& entry : m_resources) Destroy(entry);
}
Image ResourceCache::AcquireImage(const ImageDescription& d) {
    if (!d.width || !d.height || !d.levels || !d.layers || !d.usage || !d.aspect || d.format == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("Invalid cached image description");
    auto found = std::find_if(m_resources.begin(), m_resources.end(), [&](const Entry& entry) {
        const auto* image = std::get_if<Image>(&entry);
        return image && image->Matches(d);
    });
    if (found != m_resources.end()) {
        Image image = std::get<Image>(*found);
        m_cachedBytes -= image.allocationBytes;
        m_resources.erase(found);
        // Treat reuse like fresh image creation: the caller must transition and
        // initialise it. An UNDEFINED old layout explicitly discards old texels.
        image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        ++m_stats.imageReuses;
        return image;
    }
    Image image;
    image.width = d.width; image.height = d.height; image.levels = d.levels; image.layers = d.layers;
    image.format = d.format; image.aspect = d.aspect; image.usage = d.usage;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = d.layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D; info.format = d.format; info.extent = {d.width, d.height, 1};
    info.mipLevels = d.levels; info.arrayLayers = d.layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = d.usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateImage(m_device, &info, nullptr, &image.image), "vkCreateImage");
    try {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(m_device, image.image, &requirements);
        uint32_t type = m_memory.memoryTypeCount;
        for (uint32_t i = 0; i < m_memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (m_memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
        }
        if (type == m_memory.memoryTypeCount) throw std::runtime_error("Vulkan image has no device-local memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
        Check(vkAllocateMemory(m_device, &allocation, nullptr, &image.memory), "vkAllocateMemory(image)");
        image.allocationBytes = requirements.size;
        ++m_stats.imageAllocations;
        Check(vkBindImageMemory(m_device, image.image, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = d.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.format = d.format; view.subresourceRange = {d.aspect, 0, d.levels, 0, d.layers};
        Check(vkCreateImageView(m_device, &view, nullptr, &image.view), "vkCreateImageView");
    } catch (...) { DestroyImage(m_device, image); throw; }
    return image;
}
Buffer ResourceCache::AcquireBuffer(VkDeviceSize size, bool& reused) {
    if (!size) throw std::invalid_argument("Invalid cached resident buffer size");
    auto found = std::find_if(m_resources.begin(), m_resources.end(), [&](const Entry& entry) {
        const auto* buffer = std::get_if<Buffer>(&entry);
        return buffer && buffer->size == size;
    });
    reused = found != m_resources.end();
    if (reused) {
        Buffer buffer = std::get<Buffer>(*found);
        m_cachedBytes -= buffer.allocationBytes;
        m_resources.erase(found);
        ++m_stats.bufferReuses;
        return buffer;
    }
    auto buffer = CreateBuffer(m_device, m_memory, size, ResidentUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ++m_stats.bufferAllocations;
    return buffer;
}
void ResourceCache::Destroy(Entry& entry) {
    if (auto* image = std::get_if<Image>(&entry)) DestroyImage(m_device, *image);
    else DestroyBuffer(m_device, std::get<Buffer>(entry));
}
void ResourceCache::EvictOldest() {
    auto& entry = m_resources.front();
    m_cachedBytes -= std::visit([](const auto& resource) { return resource.allocationBytes; }, entry);
    Destroy(entry);
    m_resources.erase(m_resources.begin());
    ++m_stats.evictions;
}
void ResourceCache::MakeRoom(VkDeviceSize bytes) {
    while (m_cachedBytes > m_budget - bytes || m_resources.size() == EntryLimit) EvictOldest();
}
void ResourceCache::Recycle(Image& image) {
    // Samplers are state-dependent and are not part of this allocation cache.
    if (image.sampler) { vkDestroySampler(m_device, image.sampler, nullptr); image.sampler = VK_NULL_HANDLE; }
    if (!image.image || !image.allocationBytes || image.allocationBytes > m_budget) {
        DestroyImage(m_device, image);
        return;
    }
    MakeRoom(image.allocationBytes);
    m_cachedBytes += image.allocationBytes;
    m_resources.emplace_back(image);
    image = {};
}
void ResourceCache::Recycle(Buffer& buffer) {
    // Staging/readback allocations must never masquerade as resident geometry.
    if (!buffer.buffer || !buffer.allocationBytes || buffer.allocationBytes > m_budget || buffer.mapped ||
        buffer.usage != ResidentUsage || buffer.properties != VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        DestroyBuffer(m_device, buffer);
        return;
    }
    MakeRoom(buffer.allocationBytes);
    m_cachedBytes += buffer.allocationBytes;
    m_resources.emplace_back(buffer);
    buffer = {};
}
bool ResourceCache::Collect(VkFence completion, std::vector<Image>& images, std::vector<Buffer>& buffers) {
    if (!completion) throw std::invalid_argument("Resource retirement requires a completion fence");
    const auto status = vkGetFenceStatus(m_device, completion);
    if (status == VK_NOT_READY) return false;
    Check(status, "vkGetFenceStatus(resource cache)");
    for (auto& image : images) Recycle(image);
    for (auto& buffer : buffers) Recycle(buffer);
    images.clear(); buffers.clear();
    return true;
}
}
