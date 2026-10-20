#include "Engine/Source/Gx/CGxDeviceVulkan/DeviceEnumeration.h"
#include <cassert>
#include <iostream>
#include <cstdlib>

namespace {
unsigned warnings = 0, loaderNotices = 0;
VKAPI_ATTR VkBool32 VKAPI_CALL Diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        bool knownLoaderNotice = false;
        if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && type == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            && data->messageIdNumber == 0 && data->pMessageIdName && std::strcmp(data->pMessageIdName, "Loader Message") == 0) {
            for (const char* layer : {"VK_LAYER_VALVE_steam_overlay_32", "VK_LAYER_VALVE_steam_overlay_64",
                "VK_LAYER_VALVE_steam_fossilize_32", "VK_LAYER_VALVE_steam_fossilize_64", "VK_LAYER_MESA_device_select",
                "VK_LAYER_NV_optimus", "VK_LAYER_NV_present", "VK_LAYER_FROG_gamescope_wsi_x86_64", "VK_LAYER_LSFGVK_frame_generation"}) {
                const auto expected = std::string("Layer \"") + layer + "\" forced disabled because name matches filter of env var 'VK_LOADER_LAYERS_DISABLE'.";
                if (expected == data->pMessage) knownLoaderNotice = true;
            }
            if (std::strcmp(data->pMessage, "env var 'VK_INSTANCE_LAYERS' defined and adding layers \"VK_LAYER_KHRONOS_validation\"") == 0)
                knownLoaderNotice = true;
        }
        if (knownLoaderNotice) { ++loaderNotices; std::cerr << "LOADER SETUP: " << data->pMessage << '\n'; }
        else { ++warnings; std::cerr << "VALIDATION: " << data->pMessage << '\n'; }
    }
    return VK_FALSE;
}
}
int main() {
    uint32_t count = 0;
    GxVulkan::EnumerationCheck(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    GxVulkan::EnumerationCheck(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    assert(std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
        return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    }));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const char* sync1 = std::getenv("VK_LAYER_VALIDATE_SYNC");
    const char* sync2 = std::getenv("VK_VALIDATION_VALIDATE_SYNC");
    const bool sync = sync1 && std::strcmp(sync1, "1") == 0;
    assert((!sync1 && !sync2) || (sync1 && sync2 && std::strcmp(sync1, sync2) == 0));
    VkValidationFeatureEnableEXT syncFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    if (sync) { features.enabledValidationFeatureCount = 1; features.pEnabledValidationFeatures = &syncFeature; }
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = Diagnostic;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
    info.enabledLayerCount = 1; info.ppEnabledLayerNames = &layer;
    info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = extensions; info.pNext = &features; features.pNext = &debug;
    VkInstance instance = VK_NULL_HANDLE;
    GxVulkan::EnumerationCheck(vkCreateInstance(&info, nullptr, &instance));
    auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    assert(createDebug && destroyDebug);
    VkDebugUtilsMessengerEXT messenger;
    GxVulkan::EnumerationCheck(createDebug(instance, &debug, nullptr, &messenger));
    const auto candidates = GxVulkan::EnumerateAdapters(instance, VK_NULL_HANDLE);
    std::vector<GxAdapterDescriptor> descriptors;
    for (const auto& candidate : candidates) descriptors.push_back(candidate.descriptor);
    const auto automatic = GxChooseAdapter(descriptors, "auto");
    assert(automatic < candidates.size());
    for (const auto& requested : {descriptors[automatic].id, std::string("invalid-uuid")}) {
        GxConfigureDeviceSelection("vulkan", requested.c_str());
        const auto selected = GxChooseAdapter(descriptors, GxGetDeviceSelection().effectiveAdapter);
        assert(selected < candidates.size());
        const auto& candidate = candidates[selected];
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = candidate.queueFamily; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queue;
        VkDevice device = VK_NULL_HANDLE;
        GxVulkan::EnumerationCheck(vkCreateDevice(candidate.physical, &create, nullptr, &device));
        GxReportDeviceSelection("vulkan", descriptors, selected);
        VkPhysicalDeviceIDProperties uuid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 actual{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; actual.pNext = &uuid;
        vkGetPhysicalDeviceProperties2(candidate.physical, &actual);
        const auto& reported = GxGetDeviceSelection();
        assert(reported.activeAdapter == GxAdapterStableId(uuid.deviceUUID, candidate.descriptor.driverId));
        assert(reported.activeName == actual.properties.deviceName && reported.requestedAdapter == requested);
        assert(!candidate.descriptor.present);
        if (requested == "invalid-uuid") assert(!reported.warning.empty());
        std::cout << "Requested=" << requested << " actualVkPhysicalDevice=" << reported.activeAdapter
            << " name=" << reported.activeName << " driver=" << candidate.descriptor.driver << '\n';
        vkDestroyDevice(device, nullptr);
    }
    destroyDebug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    assert(warnings == 0);
    std::cout << "KHRONOS validation enabled; sync=" << sync << "; exact loader setup notices=" << loaderNotices << "; warnings/errors=0; visible devices=" << candidates.size()
        << "; headless only, presentation and multi-GPU selection not proven\n";
}
