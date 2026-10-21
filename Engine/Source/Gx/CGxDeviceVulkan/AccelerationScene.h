#ifndef GX_VULKAN_ACCELERATION_SCENE_H
#define GX_VULKAN_ACCELERATION_SCENE_H
#include "RayQueryCapabilities.h"
#include "ResidentBuffer.h"
#include <array>
#include <memory>

namespace GxVulkan {
struct ShadowPosition {
    float xyz[3];
};
struct AccelerationUsage {
    uint64_t vertices = 0, triangles = 0, physicalBytes = 0;
    uint64_t bottomBytes = 0, topBytes = 0, scratchBytes = 0;
};
// Immutable frame geometry on the existing renderer device/queue. The caller
// prepares original current poses and owns the submission fence; this class
// never loads, animates, submits, waits or owns a second device. Destroy only
// after all recorded/submitted uses complete. All geometry remains non-opaque:
// shared query code must apply authored facing/coverage, including opaque cases.
class AccelerationScene {
  public:
    AccelerationScene(VkDevice device, VkPhysicalDevice physical, const VkPhysicalDeviceMemoryProperties &memory,
                      const RayQueryCapabilities &capabilities, std::shared_ptr<GpuAdmission> admission);
    ~AccelerationScene();
    AccelerationScene(const AccelerationScene &) = delete;
    AccelerationScene &operator=(const AccelerationScene &) = delete;
    // Record outside a render pass. All validation/allocation completes before
    // the first command, so failed preparation cannot invalidate recorded draws.
    void Build(VkCommandBuffer command, const ShadowPosition *vertices, uint32_t vertexCount, const uint32_t *indices,
               uint32_t indexCount);
    VkAccelerationStructureKHR TopLevel() const;
    const AccelerationUsage &Usage() const { return m_usage; }

  private:
    enum Slot { Positions, Indices, BottomStorage, TopStorage, Instances, Scratch, SlotCount };
    VkDevice m_device;
    VkPhysicalDeviceMemoryProperties m_memory;
    std::shared_ptr<GpuAdmission> m_admission;
    std::array<Buffer, SlotCount> m_buffers{};
    VkAccelerationStructureKHR m_bottom = VK_NULL_HANDLE, m_top = VK_NULL_HANDLE;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR m_properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    PFN_vkCreateAccelerationStructureKHR m_create = nullptr;
    PFN_vkDestroyAccelerationStructureKHR m_destroy = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR m_sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR m_address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR m_build = nullptr;
    AccelerationUsage m_usage;
    bool m_attempted = false, m_built = false;
    VkDeviceAddress Address(Slot slot) const;
    void Allocate(Slot slot, VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory);
};
} // namespace GxVulkan
#endif
