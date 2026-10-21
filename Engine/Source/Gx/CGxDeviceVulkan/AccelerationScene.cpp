#include "AccelerationScene.h"
#include "GpuAdmission.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace GxVulkan {
namespace {
void Check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
template <class T> T Function(VkDevice device, const char *name) {
    auto function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
    if (!function)
        throw std::runtime_error(std::string("Unavailable acceleration device function: ") + name);
    return function;
}
} // namespace
AccelerationScene::AccelerationScene(VkDevice device, VkPhysicalDevice physical,
                                     const VkPhysicalDeviceMemoryProperties &memory,
                                     const RayQueryCapabilities &capabilities, std::shared_ptr<GpuAdmission> admission)
    : m_device(device), m_memory(memory), m_admission(std::move(admission)) {
    if (!device || !physical || !capabilities.Available() || !m_admission)
        throw std::invalid_argument("Acceleration scene requires the enabled original device and admission owner");
    m_create = Function<PFN_vkCreateAccelerationStructureKHR>(device, "vkCreateAccelerationStructureKHR");
    m_destroy = Function<PFN_vkDestroyAccelerationStructureKHR>(device, "vkDestroyAccelerationStructureKHR");
    m_sizes = Function<PFN_vkGetAccelerationStructureBuildSizesKHR>(device, "vkGetAccelerationStructureBuildSizesKHR");
    m_address =
        Function<PFN_vkGetAccelerationStructureDeviceAddressKHR>(device, "vkGetAccelerationStructureDeviceAddressKHR");
    m_build = Function<PFN_vkCmdBuildAccelerationStructuresKHR>(device, "vkCmdBuildAccelerationStructuresKHR");
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &m_properties;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if (!m_properties.minAccelerationStructureScratchOffsetAlignment)
        throw std::runtime_error("Acceleration scratch alignment unavailable");
}
AccelerationScene::~AccelerationScene() {
    if (m_top)
        m_destroy(m_device, m_top, nullptr);
    if (m_bottom)
        m_destroy(m_device, m_bottom, nullptr);
    for (auto &buffer : m_buffers)
        DestroyBuffer(m_device, buffer);
}
void AccelerationScene::Allocate(Slot slot, VkDeviceSize bytes, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags memory) {
    m_buffers[slot] = CreateBuffer(m_device, m_memory, bytes, usage, memory, m_admission.get());
    if (m_usage.physicalBytes > UINT64_MAX - m_buffers[slot].allocationBytes)
        throw std::overflow_error("Acceleration allocation census overflow");
    m_usage.physicalBytes += m_buffers[slot].allocationBytes;
}
VkDeviceAddress AccelerationScene::Address(Slot slot) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = m_buffers[slot].buffer;
    const auto address = vkGetBufferDeviceAddress(m_device, &info);
    if (!address)
        throw std::runtime_error("Acceleration buffer has no enabled device address");
    return address;
}
VkAccelerationStructureKHR AccelerationScene::TopLevel() const {
    if (!m_built)
        throw std::logic_error("Acceleration scene has not been prepared");
    return m_top;
}
void AccelerationScene::Build(VkCommandBuffer command, const ShadowPosition *vertices, uint32_t vertexCount,
                              const uint32_t *indices, uint32_t indexCount) {
    if (m_attempted)
        throw std::logic_error("Acceleration frame snapshots are immutable");
    m_attempted = true;
    if (!command || !vertices || !indices || !vertexCount || !indexCount || indexCount % 3)
        throw std::invalid_argument("Invalid acceleration frame geometry");
    const uint32_t triangles = indexCount / 3;
    if (triangles > m_properties.maxPrimitiveCount || !m_properties.maxInstanceCount)
        throw std::invalid_argument("Acceleration geometry exceeds device limits");
    for (uint32_t i = 0; i < vertexCount; ++i)
        for (float value : vertices[i].xyz)
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite acceleration position");
    for (uint32_t i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount)
            throw std::invalid_argument("Acceleration triangle index outside original geometry");
    constexpr VkBufferUsageFlags inputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags storageUsage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkMemoryPropertyFlags hostMemory =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    Allocate(Positions, uint64_t(vertexCount) * sizeof(ShadowPosition), inputUsage, hostMemory);
    Allocate(Indices, uint64_t(indexCount) * sizeof(uint32_t), inputUsage, hostMemory);
    std::memcpy(m_buffers[Positions].mapped, vertices, m_buffers[Positions].size);
    std::memcpy(m_buffers[Indices].mapped, indices, m_buffers[Indices].size);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto &triangle = geometry.geometry.triangles;
    triangle.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangle.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangle.vertexStride = sizeof(ShadowPosition);
    triangle.maxVertex = vertexCount - 1;
    triangle.vertexData.deviceAddress = Address(Positions);
    triangle.indexType = VK_INDEX_TYPE_UINT32;
    triangle.indexData.deviceAddress = Address(Indices);
    VkAccelerationStructureBuildGeometryInfoKHR bottom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bottom.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bottom.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bottom.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bottom.geometryCount = 1;
    bottom.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR bottomSize{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_sizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bottom, &triangles, &bottomSize);
    Allocate(BottomStorage, bottomSize.accelerationStructureSize, storageUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.type = bottom.type;
    create.buffer = m_buffers[BottomStorage].buffer;
    create.size = bottomSize.accelerationStructureSize;
    Check(m_create(m_device, &create, nullptr, &m_bottom), "vkCreateAccelerationStructureKHR(bottom)");
    VkAccelerationStructureDeviceAddressInfoKHR bottomAddress{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    bottomAddress.accelerationStructure = m_bottom;
    const auto reference = m_address(m_device, &bottomAddress);
    if (!reference)
        throw std::runtime_error("Bottom-level acceleration address unavailable");
    Allocate(Instances, sizeof(VkAccelerationStructureInstanceKHR), inputUsage, hostMemory);
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    instance.mask = 0xff;
    instance.accelerationStructureReference = reference;
    std::memcpy(m_buffers[Instances].mapped, &instance, sizeof(instance));
    VkAccelerationStructureGeometryKHR instances{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    instances.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    instances.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instances.geometry.instances.data.deviceAddress = Address(Instances);
    VkAccelerationStructureBuildGeometryInfoKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    top.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    top.flags = bottom.flags;
    top.mode = bottom.mode;
    top.geometryCount = 1;
    top.pGeometries = &instances;
    const uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR topSize{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_sizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &top, &instanceCount, &topSize);
    Allocate(TopStorage, topSize.accelerationStructureSize, storageUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    create.type = top.type;
    create.buffer = m_buffers[TopStorage].buffer;
    create.size = topSize.accelerationStructureSize;
    Check(m_create(m_device, &create, nullptr, &m_top), "vkCreateAccelerationStructureKHR(top)");
    const auto scratchBytes =
        bottomSize.buildScratchSize > topSize.buildScratchSize ? bottomSize.buildScratchSize : topSize.buildScratchSize;
    const VkDeviceSize alignment = m_properties.minAccelerationStructureScratchOffsetAlignment;
    if (scratchBytes > UINT64_MAX - (alignment - 1))
        throw std::overflow_error("Acceleration scratch size overflow");
    Allocate(Scratch, scratchBytes + alignment - 1,
             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto scratch = Address(Scratch);
    if (scratch > UINT64_MAX - (alignment - 1))
        throw std::overflow_error("Acceleration scratch address overflow");
    const auto alignedScratch = (scratch + alignment - 1) & ~VkDeviceAddress(alignment - 1);
    bottom.dstAccelerationStructure = m_bottom;
    bottom.scratchData.deviceAddress = alignedScratch;
    top.dstAccelerationStructure = m_top;
    top.scratchData.deviceAddress = alignedScratch;
    m_usage.vertices = vertexCount;
    m_usage.triangles = triangles;
    m_usage.bottomBytes = bottomSize.accelerationStructureSize;
    m_usage.topBytes = topSize.accelerationStructureSize;
    m_usage.scratchBytes = scratchBytes + alignment - 1;
    // No operations below allocate or throw. All resources survive the caller's
    // frame fence even if a later material/receiver gate disables queries.
    VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    ready.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0,
                         1, &ready, 0, nullptr, 0, nullptr);
    VkAccelerationStructureBuildRangeInfoKHR bottomRange{};
    bottomRange.primitiveCount = triangles;
    const auto *bottomRanges = &bottomRange;
    m_build(command, 1, &bottom, &bottomRanges);
    VkMemoryBarrier buildReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    buildReady.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    buildReady.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &buildReady, 0, nullptr, 0,
                         nullptr);
    VkAccelerationStructureBuildRangeInfoKHR topRange{};
    topRange.primitiveCount = 1;
    const auto *topRanges = &topRange;
    m_build(command, 1, &top, &topRanges);
    buildReady.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &buildReady, 0, nullptr, 0, nullptr);
    m_built = true;
}
} // namespace GxVulkan
