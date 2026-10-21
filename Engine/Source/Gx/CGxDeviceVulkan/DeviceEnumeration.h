#pragma once
#include "Engine/Source/Gx/DeviceSelection.h"
#include <vulkan/vulkan.h>
#include <cstring>
#include <stdexcept>

namespace GxVulkan {
struct AdapterCandidate {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    GxAdapterDescriptor descriptor;
};
inline void EnumerationCheck(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan device enumeration failed (VkResult " + std::to_string(result) + ")");
}
// A null surface is a headless probe, never proof of presentation support.
inline std::vector<AdapterCandidate> EnumerateAdapters(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    EnumerationCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (count > 64) throw std::runtime_error("Vulkan device count exceeds selector limit");
    std::vector<VkPhysicalDevice> devices(count);
    EnumerationCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    devices.resize(count);
    std::vector<AdapterCandidate> result;
    for (auto physical : devices) {
        AdapterCandidate candidate; candidate.physical = physical;
        auto& d = candidate.descriptor;
        uint32_t extensionCount = 0;
        EnumerationCheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr));
        std::vector<VkExtensionProperties> extensions(extensionCount);
        EnumerationCheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data()));
        const auto has = [&](const char* name) { return std::any_of(extensions.begin(), extensions.end(),
            [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; }); };
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        if (properties.apiVersion >= VK_API_VERSION_1_2 || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) identity.pNext = &driver;
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &identity;
        vkGetPhysicalDeviceProperties2(physical, &props);
        d.driverId = static_cast<uint32_t>(driver.driverID);
        std::copy(std::begin(identity.driverUUID), std::end(identity.driverUUID), d.driverUUID.begin());
        d.vendorId = properties.vendorID; d.deviceId = properties.deviceID;
        d.id = GxAdapterStableId(identity.deviceUUID, d.driverId);
        d.name.assign(properties.deviceName, strnlen(properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE));
        d.driver.assign(driver.driverName, strnlen(driver.driverName, VK_MAX_DRIVER_NAME_SIZE));
        if (d.driver.empty()) d.driver = "Driver identity unavailable";
        d.swapchain = has(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
        bool graphicsPresent = false;
        for (uint32_t family = 0; family < queueCount; ++family) {
            if (!queues[family].queueCount || !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            d.graphics = true;
            VkBool32 present = false;
            if (surface) EnumerationCheck(vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &present));
            if (present || !surface) { candidate.queueFamily = family; graphicsPresent = true; }
            if (present) d.present = true;
        }
        uint32_t formats = 0, modes = 0;
        if (surface && graphicsPresent) {
            EnumerationCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, nullptr));
            EnumerationCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modes, nullptr));
        }
        d.compatible = surface ? d.IsPresentationCompatible(properties.apiVersion >= VK_API_VERSION_1_1, formats != 0, modes != 0)
            : properties.apiVersion >= VK_API_VERSION_1_1 && graphicsPresent && d.swapchain;
        d.reason = d.compatible ? (surface ? "Vulkan 1.1; graphics/present queue; swapchain formats and modes verified" : "Headless graphics probe; presentation unverified")
            : "Requires Vulkan 1.1, graphics/present queue, swapchain extension, surface formats and modes";
        result.push_back(std::move(candidate));
    }
    return result;
}
}
