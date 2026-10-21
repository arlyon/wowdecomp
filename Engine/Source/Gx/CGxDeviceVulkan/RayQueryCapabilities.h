#ifndef GX_VULKAN_RAY_QUERY_CAPABILITIES_H
#define GX_VULKAN_RAY_QUERY_CAPABILITIES_H
#include <array>
#include <cstddef>
namespace GxVulkan {
// Capability only, never material/scene readiness. Defaults fail closed.
enum class RayQueryRequirement : size_t {
    Core12,
    ComputeQueue,
    AccelerationExtension,
    QueryExtension,
    DeferredHostExtension,
    BufferAddress,
    AccelerationFeature,
    QueryFeature,
    RuntimeTextureArray,
    NonUniformTextureIndex,
    VariableTextureCount,
    Count
};
constexpr size_t RayQueryRequirementCount = static_cast<size_t>(RayQueryRequirement::Count);
struct RayQueryCapabilities {
    std::array<bool, RayQueryRequirementCount> supported{};
    void Set(RayQueryRequirement requirement, bool value) { supported.at(static_cast<size_t>(requirement)) = value; }
    RayQueryRequirement Missing() const {
        for (size_t i = 0; i < supported.size(); ++i)
            if (!supported[i])
                return static_cast<RayQueryRequirement>(i);
        return RayQueryRequirement::Count;
    }
    bool Available() const { return Missing() == RayQueryRequirement::Count; }
    const char *Reason() const {
        constexpr std::array<const char *, RayQueryRequirementCount + 1> names = {
            "Vulkan 1.2",
            "compute-capable graphics queue",
            "VK_KHR_acceleration_structure",
            "VK_KHR_ray_query",
            "VK_KHR_deferred_host_operations",
            "bufferDeviceAddress",
            "accelerationStructure",
            "rayQuery",
            "runtimeDescriptorArray",
            "shaderSampledImageArrayNonUniformIndexing",
            "descriptorBindingVariableDescriptorCount",
            "available"};
        return names.at(static_cast<size_t>(Missing()));
    }
};
} // namespace GxVulkan
#endif
