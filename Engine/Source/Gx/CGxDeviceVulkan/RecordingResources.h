#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RECORDINGRESOURCES_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_RECORDINGRESOURCES_H

#include "ResidentBuffer.h"
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace GxVulkan {
// One recording lane on a borrowed device. No queue, swapchain, device factory or
// engine globals: separate lanes must never reset each other's transient storage.
// The owner must finish GPU use before destruction, including failed submissions.
struct RecordingResources {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    Buffer upload;
    VkDeviceSize uploadOffset = 0;
    std::vector<VkFramebuffer> framebuffers;

    RecordingResources() = default;
    RecordingResources(const RecordingResources&) = delete;
    RecordingResources& operator=(const RecordingResources&) = delete;
    ~RecordingResources() { Destroy(); }

    void Initialize(VkDevice device, uint32_t queueFamily,
        const VkPhysicalDeviceMemoryProperties& memory, VkDeviceSize uploadBytes = 64ull * 1024 * 1024) {
        if (owner || !device || !uploadBytes) throw std::logic_error("Invalid recording resource initialization");
        owner = device;
        try {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = queueFamily; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            Check(vkCreateCommandPool(owner, &pool, nullptr, &commandPool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = commandPool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
            Check(vkAllocateCommandBuffers(owner, &allocate, &command), "vkAllocateCommandBuffers");
            VkFenceCreateInfo completion{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; completion.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            Check(vkCreateFence(owner, &completion, nullptr, &fence), "vkCreateFence");
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 98304},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 327680}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
                {VK_DESCRIPTOR_TYPE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16400}};
            VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            descriptors.maxSets = 65536; descriptors.poolSizeCount = 5; descriptors.pPoolSizes = sizes;
            Check(vkCreateDescriptorPool(owner, &descriptors, nullptr, &descriptorPool), "vkCreateDescriptorPool");
            upload = CreateBuffer(owner, memory, uploadBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        } catch (...) { Destroy(); throw; }
    }

    void Swap(RecordingResources& other) noexcept {
        using std::swap;
        swap(owner,other.owner);swap(commandPool,other.commandPool);swap(command,other.command);
        swap(fence,other.fence);swap(descriptorPool,other.descriptorPool);swap(upload,other.upload);
        swap(uploadOffset,other.uploadOffset);framebuffers.swap(other.framebuffers);
    }

    bool Complete() const {
        if (!owner || !fence) throw std::logic_error("Recording resources are not initialized");
        const auto result = vkGetFenceStatus(owner, fence);
        if (result == VK_NOT_READY) return false;
        Check(result, "vkGetFenceStatus");
        return true;
    }

    // A readiness poll, never a wait. This also rejects a fence reset for an
    // unsubmitted generation, rather than recycling storage behind its owner.
    bool TryReset() {
        if (!Complete()) return false;
        Check(vkResetCommandPool(owner, commandPool, 0), "vkResetCommandPool");
        ClearFramebuffers();
        Check(vkResetDescriptorPool(owner, descriptorPool, 0), "vkResetDescriptorPool");
        uploadOffset = 0;
        return true;
    }

    void ClearFramebuffers() {
        for (auto framebuffer : framebuffers) vkDestroyFramebuffer(owner, framebuffer, nullptr);
        framebuffers.clear();
    }

    // Explicit teardown lets a device owner release lanes before vkDestroyDevice.
    // This intentionally performs no device-idle or application-controlled wait.
    void Destroy() noexcept {
        if (!owner) return;
        if (commandPool) vkDestroyCommandPool(owner, commandPool, nullptr);
        ClearFramebuffers();
        if (descriptorPool) vkDestroyDescriptorPool(owner, descriptorPool, nullptr);
        DestroyBuffer(owner, upload);
        if (fence) vkDestroyFence(owner, fence, nullptr);
        commandPool = VK_NULL_HANDLE; command = VK_NULL_HANDLE; fence = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE; uploadOffset = 0; owner = VK_NULL_HANDLE;
    }

private:
    VkDevice owner = VK_NULL_HANDLE;
    static void Check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
    }
};
}
#endif
