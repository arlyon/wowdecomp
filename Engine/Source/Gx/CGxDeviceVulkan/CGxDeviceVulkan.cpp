#include "CGxDeviceVulkan.h"
#include "Shader.h"
#include "Slang.h"
#include "ResidentBuffer.h"
#include "Coordinates.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/Buffer.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/SDL/Window.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void VkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
#define GX_VK(call) VkCheck((call), #call)

std::vector<uint32_t> ReadModule(const char* filename) {
    std::ifstream stream(std::string(WOWDECOMP_VULKAN_SHADER_DIR) + "/" + filename, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (!stream || size < 20 || size % 4 || size > 4 * 1024 * 1024)
        throw std::runtime_error(std::string("Invalid Slang shader module: ") + filename);
    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), size);
    if (!stream || code[0] != 0x07230203) throw std::runtime_error("Invalid SPIR-V shader header");
    return code;
}

VkFormat TextureFormat(EGxTexFormat format) {
    static constexpr VkFormat formats[] = {
        VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_UNDEFINED, VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_R8G8_SNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_D32_SFLOAT
    };
    return format > GxTex_Unknown && format < GxTexFormats_Last ? formats[format] : VK_FORMAT_UNDEFINED;
}

using NativeBuffer = GxVulkan::Buffer;
struct CachedGeometry {
    CGxPool* pool = nullptr;
    uint32_t index = 0, bytes = 0;
    GxVulkan::ResidentBuffer resident;
};
struct NativeImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct NativeShader {
    uint64_t id;
    GxVulkan::Shader reflection;
    std::vector<uint32_t> code;
};
struct Pipeline {
    VkPipeline handle = VK_NULL_HANDLE;
};

void WindowEvent(void* context, const SDL_WindowEvent& event) {
    auto& device = *static_cast<CGxDeviceVulkan*>(context);
    switch (event.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        case SDL_WINDOWEVENT_RESTORED:
            device.DeviceWM(GxWM_Size, 0, 0);
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: device.DeviceWM(GxWM_SetFocus, 0, 0); break;
        case SDL_WINDOWEVENT_FOCUS_LOST: device.DeviceWM(GxWM_KillFocus, 0, 0); break;
        case SDL_WINDOWEVENT_MINIMIZED: device.DeviceWM(GxWM_Size, 0, 1); break;
        default: break;
    }
}
}

struct CGxDeviceVulkan::State {
    explicit State(CGxDeviceVulkan& device) : gx(device) {}
    CGxDeviceVulkan& gx;
    SDL_Window* window = nullptr;
    bool ownWindow = false, resize = false, recording = false, inPass = false, acquirePending = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    uint32_t imageIndex = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    // Presentation completion is not covered by the submit fence. Reuse each
    // render-finished semaphore only after acquiring its swapchain image.
    std::vector<VkSemaphore> complete;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, 4> descriptorLayouts{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    NativeBuffer upload;
    VkDeviceSize uploadOffset = 0;
    NativeImage depth, white;
    std::map<CGxPool*, std::vector<uint8_t>> pools;
    std::map<CGxBuf*, CachedGeometry> geometry;
    std::vector<NativeBuffer> retiredBuffers;
    uint64_t bufferFrame = 0, residentUploadBytes = 0;
    std::map<CGxTex*, NativeImage> textures;
    // Samplers describe filtering, not texture identity. In particular Metal
    // permits far fewer live sampler objects than a world has textures.
    std::map<std::array<uint32_t, 7>, VkSampler> samplers;
    VkSampler Sampler(const VkSamplerCreateInfo& info);
    std::vector<NativeImage> retiredImages;
    std::map<CGxShader*, NativeShader> shaders;
    uint64_t nextShader = 1;
    GxVulkan::SlangCompiler shaderCompiler;
    std::vector<uint32_t> fixedVertex, fixedPixel;
    std::map<std::vector<uint64_t>, Pipeline> pipelines;
    std::map<std::pair<VkFormat, VkFormat>, VkRenderPass> renderPasses;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass currentPass = VK_NULL_HANDLE;
    VkExtent2D targetExtent{};

    uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const;
    NativeBuffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(NativeBuffer& buffer);
    NativeImage MakeImage(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
        VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage);
    void DestroyImage(NativeImage& image);
    void Barrier(VkImage image, VkImageAspectFlags aspect, uint32_t levels, uint32_t layers,
        VkImageLayout oldLayout, VkImageLayout newLayout);
    VkDeviceSize Upload(const void* bytes, size_t size, size_t alignment = 16);
    std::pair<VkBuffer, VkDeviceSize> Geometry(CGxBuf* buffer);
    void RetireGeometry(CGxBuf* buffer);
    void Initialize();
    void Destroy();
    void RecreateSwapchain();
    bool BeginFrame();
    void Submit(bool present, bool resume);
    void EndPass();
    void BeginPass();
    NativeImage& Texture(CGxTex* texture);
    void UploadTexture(CGxTex* texture, NativeImage& image);
    void Draw(CGxBatch& batch, bool indexed);
};

uint32_t CGxDeviceVulkan::State::MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("Vulkan device has no suitable memory type");
}
NativeBuffer CGxDeviceVulkan::State::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    return GxVulkan::CreateBuffer(device, memoryProperties, size, usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}
void CGxDeviceVulkan::State::DestroyBuffer(NativeBuffer& buffer) {
    GxVulkan::DestroyBuffer(device, buffer);
}
NativeImage CGxDeviceVulkan::State::MakeImage(uint32_t width, uint32_t height, uint32_t levels,
    uint32_t layers, VkFormat format, VkImageAspectFlags aspect, VkImageUsageFlags usage) {
    NativeImage image;
    image.width = width; image.height = height; image.levels = levels; image.layers = layers;
    image.format = format; image.aspect = aspect;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = levels; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    GX_VK(vkCreateImage(device, &info, nullptr, &image.image));
    try {
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        GX_VK(vkAllocateMemory(device, &allocation, nullptr, &image.memory));
        GX_VK(vkBindImageMemory(device, image.image, image.memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.format = format; view.subresourceRange = {aspect, 0, levels, 0, layers};
        GX_VK(vkCreateImageView(device, &view, nullptr, &image.view));
    } catch (...) { DestroyImage(image); throw; }
    return image;
}
VkSampler CGxDeviceVulkan::State::Sampler(const VkSamplerCreateInfo& info) {
    uint32_t maxLod;
    static_assert(sizeof(maxLod) == sizeof(info.maxLod));
    std::memcpy(&maxLod, &info.maxLod, sizeof(maxLod));
    // All other VkSamplerCreateInfo fields are identical (zero/default) for
    // the two callers. Include each field they vary in the cache key.
    const std::array<uint32_t, 7> key{info.magFilter, info.minFilter, info.mipmapMode,
        info.addressModeU, info.addressModeV, info.addressModeW, maxLod};
    auto found = samplers.find(key);
    if (found != samplers.end()) return found->second;
    if (samplers.size() >= properties.limits.maxSamplerAllocationCount)
        throw std::runtime_error("Vulkan unique sampler state limit exhausted");
    VkSampler sampler;
    GX_VK(vkCreateSampler(device, &info, nullptr, &sampler));
    try { samplers.emplace(key, sampler); }
    catch (...) { vkDestroySampler(device, sampler, nullptr); throw; }
    return sampler;
}
void CGxDeviceVulkan::State::DestroyImage(NativeImage& image) {
    // The device cache owns samplers; retired images may still reference them.
    if (image.view) vkDestroyImageView(device, image.view, nullptr);
    if (image.image) vkDestroyImage(device, image.image, nullptr);
    if (image.memory) vkFreeMemory(device, image.memory, nullptr);
    image = {};
}
void CGxDeviceVulkan::State::Barrier(VkImage image, VkImageAspectFlags aspect,
    uint32_t levels, uint32_t layers, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {aspect, 0, levels, 0, layers};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}
VkDeviceSize CGxDeviceVulkan::State::Upload(const void* bytes, size_t size, size_t alignment) {
    const VkDeviceSize offset = (uploadOffset + alignment - 1) / alignment * alignment;
    if (size > upload.size || offset > upload.size - size) {
        GxLog("Native Vulkan upload budget exhausted: requested=%llu used=%llu capacity=%llu resident_upload=%llu",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(uploadOffset),
            static_cast<unsigned long long>(upload.size), static_cast<unsigned long long>(residentUploadBytes));
        throw std::runtime_error("Vulkan per-frame upload budget exhausted");
    }
    if (size) std::memcpy(upload.mapped + offset, bytes, size);
    uploadOffset = offset + size;
    return offset;
}

void CGxDeviceVulkan::State::Initialize() {
    unsigned extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) throw std::runtime_error(SDL_GetError());
    std::vector<const char*> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) throw std::runtime_error(SDL_GetError());
    uint32_t count = 0;
    GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> available(count);
    GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()));
    const bool portability = std::any_of(available.begin(), available.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
    });
    if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    std::vector<const char*> layers;
    if (const char* validation = std::getenv("WOWDECOMP_VULKAN_VALIDATION"); validation && std::strcmp(validation, "1") == 0) {
        GX_VK(vkEnumerateInstanceLayerProperties(&count, nullptr));
        std::vector<VkLayerProperties> availableLayers(count);
        GX_VK(vkEnumerateInstanceLayerProperties(&count, availableLayers.data()));
        if (std::none_of(availableLayers.begin(), availableLayers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        })) throw std::runtime_error("Requested Vulkan validation layer is unavailable");
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "wowdecomp"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app; info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data(); info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (portability) info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    GX_VK(vkCreateInstance(&info, nullptr, &instance));
    if (!SDL_Vulkan_CreateSurface(window, instance, &surface)) throw std::runtime_error(SDL_GetError());
    GX_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    GX_VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties candidateProperties;
        vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
        if (candidateProperties.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t family = 0; family < queueCount; ++family) {
            VkBool32 present = false;
            GX_VK(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &present));
            if (present && queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical = candidate; queueFamily = family; break;
            }
        }
        if (physical) break;
    }
    if (!physical) throw std::runtime_error("No Vulkan 1.1 graphics/present device");
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    GX_VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    available.resize(count);
    GX_VK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()));
    extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (const auto& extension : available)
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
            extensions.push_back("VK_KHR_portability_subset");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = features.textureCompressionBC;
    enabled.largePoints = features.largePoints;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queueInfo; create.pEnabledFeatures = &enabled;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size()); create.ppEnabledExtensionNames = extensions.data();
    GX_VK(vkCreateDevice(physical, &create, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamily; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    GX_VK(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commandPool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
    GX_VK(vkAllocateCommandBuffers(device, &allocate, &command));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    GX_VK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    GX_VK(vkCreateSemaphore(device, &semaphore, nullptr, &acquired));
    for (unsigned set = 0; set < descriptorLayouts.size(); ++set) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        const bool uniform = set % 2;
        for (unsigned binding = 0; binding < (uniform ? (set == 3 ? 2u : 1u) : set == 0 ? 4u : 16u); ++binding)
            bindings.push_back({binding, uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, set < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<uint32_t>(bindings.size()); layout.pBindings = bindings.data();
        GX_VK(vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptorLayouts[set]));
    }
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = static_cast<uint32_t>(descriptorLayouts.size()); layout.pSetLayouts = descriptorLayouts.data();
    GX_VK(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 327680}};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets = 65536; descriptors.poolSizeCount = 2; descriptors.pPoolSizes = sizes;
    GX_VK(vkCreateDescriptorPool(device, &descriptors, nullptr, &descriptorPool));
    upload = MakeBuffer(64 * 1024 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    fixedVertex = ReadModule("fixed.vert.spv"); fixedPixel = ReadModule("fixed.frag.spv");
    RecreateSwapchain();
    CGxDevice::Log("Native Vulkan: %s, API %u.%u; direct Vulkan submission (no DXVK)", properties.deviceName,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
}

void CGxDeviceVulkan::State::RecreateSwapchain() {
    // SDL caches window geometry in its event pump. Synchronise it before the
    // initial viewport/font measurements as well as subsequent resizes.
    SDL_PumpEvents();
    GX_VK(vkDeviceWaitIdle(device));
    for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (auto view : swapViews) vkDestroyImageView(device, view, nullptr);
    swapViews.clear(); swapImages.clear();
    for (auto semaphore : complete) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
    complete.clear();
    DestroyImage(depth);
    VkSurfaceCapabilitiesKHR capabilities;
    GX_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities));
    uint32_t count = 0;
    GX_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    GX_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
    if (formats.empty()) throw std::runtime_error("Vulkan surface has no colour formats");
    auto chosen = formats.front();
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = format;
    if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
    surfaceFormat = chosen.format;
    int width, height;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 1)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 1)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Vulkan swapchain does not support screenshot readback");
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface; create.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount) create.minImageCount = std::min(create.minImageCount, capabilities.maxImageCount);
    create.imageFormat = chosen.format; create.imageColorSpace = chosen.colorSpace; create.imageExtent = extent;
    create.imageArrayLayers = 1; create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & create.compositeAlpha))
        for (auto flag : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & flag) { create.compositeAlpha = flag; break; }
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = true; create.oldSwapchain = swapchain;
    VkSwapchainKHR next;
    GX_VK(vkCreateSwapchainKHR(device, &create, nullptr, &next));
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = next;
    GX_VK(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
    swapImages.resize(count); swapViews.resize(count);
    complete.resize(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& finished : complete) GX_VK(vkCreateSemaphore(device, &semaphore, nullptr, &finished));
    GX_VK(vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data()));
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapImages[i]; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = surfaceFormat;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        GX_VK(vkCreateImageView(device, &view, nullptr, &swapViews[i]));
    }
    depth = MakeImage(extent.width, extent.height, 1, 1, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    CRect rect;
    rect.minX = rect.minY = 0; rect.maxX = static_cast<float>(extent.width); rect.maxY = static_cast<float>(extent.height);
    gx.DeviceSetDefWindow(rect); gx.DeviceSetCurWindow(rect);
    gx.m_format.m_size = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height)};
    gx.m_viewportDirty = 1; resize = false;
}

bool CGxDeviceVulkan::State::BeginFrame() {
    if (recording) return true;
    int width, height;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) return false;
    GX_VK(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
    for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (auto& image : retiredImages) DestroyImage(image);
    retiredImages.clear();
    for (auto& buffer : retiredBuffers) DestroyBuffer(buffer);
    retiredBuffers.clear();
    if (resize) RecreateSwapchain();
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &imageIndex);
    }
    if (result == VK_SUBOPTIMAL_KHR) resize = true;
    else VkCheck(result, "vkAcquireNextImageKHR");
    GX_VK(vkResetCommandPool(device, commandPool, 0));
    GX_VK(vkResetDescriptorPool(device, descriptorPool, 0));
    uploadOffset = 0;
    residentUploadBytes = 0;
    ++bufferFrame;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    GX_VK(vkBeginCommandBuffer(command, &begin));
    recording = true; acquirePending = true;
    Barrier(swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    if (depth.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        Barrier(depth.image, depth.aspect, 1, 1, depth.layout, VK_IMAGE_LAYOUT_GENERAL);
        depth.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    return true;
}
void CGxDeviceVulkan::State::EndPass() {
    if (!inPass) return;
    vkCmdEndRenderPass(command); inPass = false;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void CGxDeviceVulkan::State::BeginPass() {
    if (inPass) return;
    auto& colourTarget = gx.m_textureTarget[GxBuffers_Color];
    auto& depthTarget = gx.m_textureTarget[GxBuffers_Depth];
    NativeImage* colourImage = colourTarget.m_texture ? &Texture(colourTarget.m_texture) : nullptr;
    NativeImage* depthImage = depthTarget.m_texture ? &Texture(depthTarget.m_texture) : &depth;
    if (colourTarget.m_plane || depthTarget.m_plane)
        throw std::runtime_error("Vulkan cubemap render-target plane is not implemented");
    targetExtent = colourImage ? VkExtent2D{colourImage->width, colourImage->height} : extent;
    if (depthImage->width < targetExtent.width || depthImage->height < targetExtent.height)
        throw std::runtime_error("Vulkan depth target is smaller than the colour target");
    const VkFormat colourFormat = colourImage ? colourImage->format : surfaceFormat;
    const auto key = std::make_pair(colourFormat, depthImage->format);
    auto found = renderPasses.find(key);
    if (found == renderPasses.end()) {
        VkAttachmentDescription attachments[2]{};
        for (unsigned i = 0; i < 2; ++i) {
            attachments[i].format = i ? depthImage->format : colourFormat;
            attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[i].initialLayout = attachments[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_GENERAL}, depthReference{1, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colour; subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
        dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        create.attachmentCount = 2; create.pAttachments = attachments;
        create.subpassCount = 1; create.pSubpasses = &subpass; create.dependencyCount = 1; create.pDependencies = &dependency;
        VkRenderPass pass;
        GX_VK(vkCreateRenderPass(device, &create, nullptr, &pass));
        found = renderPasses.emplace(key, pass).first;
    }
    currentPass = found->second;
    const VkImageView views[] = {colourImage ? colourImage->view : swapViews[imageIndex], depthImage->view};
    VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create.renderPass = currentPass; create.attachmentCount = 2; create.pAttachments = views;
    create.width = targetExtent.width; create.height = targetExtent.height; create.layers = 1;
    VkFramebuffer framebuffer;
    GX_VK(vkCreateFramebuffer(device, &create, nullptr, &framebuffer));
    framebuffers.push_back(framebuffer);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = currentPass; begin.framebuffer = framebuffer; begin.renderArea.extent = targetExtent;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    inPass = true;
}

void CGxDeviceVulkan::State::Destroy() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (commandPool) vkResetCommandPool(device, commandPool, 0);
        recording = inPass = false;
        for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
        for (auto& pipeline : pipelines) vkDestroyPipeline(device, pipeline.second.handle, nullptr);
        pipelines.clear();
        for (auto& pass : renderPasses) vkDestroyRenderPass(device, pass.second, nullptr);
        renderPasses.clear();
        for (auto& entry : textures) {
            entry.first->m_apiSpecificData = nullptr;
            entry.first->m_needsCreation = 1;
            DestroyImage(entry.second);
        }
        textures.clear();
        for (auto& image : retiredImages) DestroyImage(image);
        retiredImages.clear();
        for (auto& entry : geometry) entry.second.resident.Destroy(device);
        geometry.clear();
        for (auto& buffer : retiredBuffers) DestroyBuffer(buffer);
        retiredBuffers.clear();
        DestroyImage(white); DestroyImage(depth); DestroyBuffer(upload);
        for (const auto& entry : samplers) vkDestroySampler(device, entry.second, nullptr);
        samplers.clear();
        for (auto view : swapViews) if (view) vkDestroyImageView(device, view, nullptr);
        swapViews.clear(); swapImages.clear();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        for (auto layout : descriptorLayouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (acquired) vkDestroySemaphore(device, acquired, nullptr);
        for (auto semaphore : complete) if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        complete.clear();
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
    if (OsGuiGetWindow(0) == window) OsGuiSetGxWindow(nullptr);
    if (window && ownWindow) SDL_DestroyWindow(window);
    window = nullptr;
}

NativeImage& CGxDeviceVulkan::State::Texture(CGxTex* texture) {
    if (!texture) {
        if (!white.image) {
            EndPass();
            white = MakeImage(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Barrier(white.image, white.aspect, 1, 1, white.layout, VK_IMAGE_LAYOUT_GENERAL);
            white.layout = VK_IMAGE_LAYOUT_GENERAL;
            const uint32_t pixel = 0xFFFFFFFF;
            VkBufferImageCopy copy{};
            copy.bufferOffset = Upload(&pixel, sizeof(pixel));
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(command, upload.buffer, white.image, white.layout, 1, &copy);
            Barrier(white.image, white.aspect, 1, 1, white.layout, white.layout);
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            white.sampler = Sampler(sampler);
        }
        return white;
    }
    auto found = textures.find(texture);
    if (found == textures.end() || texture->m_needsCreation) {
        EndPass();
        if (found != textures.end()) {
            retiredImages.push_back(found->second); textures.erase(found);
        }
        uint32_t width, height, baseMip, mipCount;
        gx.ITexWHDStartEnd(texture, width, height, baseMip, mipCount);
        VkFormat format = TextureFormat(texture->m_format);
        VkImageAspectFlags aspect = texture->m_format == GxTex_D24X8 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (texture->m_flags.m_renderTarget) usage |= aspect == VK_IMAGE_ASPECT_DEPTH_BIT ?
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const uint32_t levels = texture->m_flags.m_renderTarget ? 1 : mipCount - baseMip;
        NativeImage image = MakeImage(width, height, std::max(levels, 1u), texture->m_target == GxTex_CubeMap ? 6 : 1,
            format, aspect, usage);
        Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, VK_IMAGE_LAYOUT_GENERAL);
        image.layout = VK_IMAGE_LAYOUT_GENERAL;
        found = textures.emplace(texture, image).first;
        texture->m_apiSpecificData = &found->second;
        texture->m_needsCreation = 0; texture->m_needsFlagUpdate = 1;
    }
    NativeImage& image = found->second;
    if (!image.sampler || texture->m_needsFlagUpdate) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = texture->m_flags.m_filter == GxTex_Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.minFilter = texture->m_flags.m_filter == GxTex_Nearest || texture->m_flags.m_filter == GxTex_NearestMipNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sampler.mipmapMode = texture->m_flags.m_filter >= GxTex_LinearMipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = texture->m_flags.m_wrapU ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = texture->m_flags.m_wrapV ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = static_cast<float>(image.levels - 1);
        image.sampler = Sampler(sampler);
        texture->m_needsFlagUpdate = 0;
    }
    if (texture->m_needsUpdate) {
        if (texture->m_userFunc && !texture->m_flags.m_renderTarget) UploadTexture(texture, image);
        gx.CGxDevice::ITexMarkAsUpdated(texture);
    }
    return image;
}
void CGxDeviceVulkan::State::UploadTexture(CGxTex* texture, NativeImage& image) {
    EndPass();
    uint32_t stride = 0;
    const void* texels = nullptr;
    texture->m_userFunc(GxTex_Lock, texture->m_width, texture->m_height, 0, 0, texture->m_userArg, stride, texels);
    struct Unlock {
        CGxTex* texture;
        ~Unlock() {
            uint32_t stride = 0; const void* texels = nullptr;
            texture->m_userFunc(GxTex_Unlock, texture->m_width, texture->m_height, 0, 0,
                texture->m_userArg, stride, texels);
        }
    } unlock{texture};
    uint32_t width, height, baseMip, mipCount;
    gx.ITexWHDStartEnd(texture, width, height, baseMip, mipCount);
    Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
    for (uint32_t face = 0; face < image.layers; ++face) {
        for (uint32_t level = 0; level < image.levels; ++level) {
            const uint32_t mip = baseMip + level;
            const uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
            texels = nullptr;
            texture->m_userFunc(GxTex_Latch, w, h, face, mip, texture->m_userArg, stride, texels);
            if (!texels) throw std::runtime_error("Texture upload callback returned no texels");
            uint32_t left = std::clamp(texture->m_updateRect.minX >> mip, 0, static_cast<int>(w));
            uint32_t top = std::clamp(texture->m_updateRect.minY >> mip, 0, static_cast<int>(h));
            uint32_t right = std::clamp(std::max(texture->m_updateRect.maxX >> mip, static_cast<int>(left + 1)), 0, static_cast<int>(w));
            uint32_t bottom = std::clamp(std::max(texture->m_updateRect.maxY >> mip, static_cast<int>(top + 1)), 0, static_cast<int>(h));
            const bool compressed = texture->m_format >= GxTex_Dxt1 && texture->m_format <= GxTex_Dxt5;
            if (compressed) { left &= ~3u; top &= ~3u; right = std::min((right + 3) & ~3u, w); bottom = std::min((bottom + 3) & ~3u, h); }
            if (left >= right || top >= bottom) continue;
            const bool sourceCompressed = texture->m_dataFormat >= GxTex_Dxt1 && texture->m_dataFormat <= GxTex_Dxt5;
            auto source = static_cast<const uint8_t*>(texels);
            if (!texture->m_flags.m_bit15) {
                source += sourceCompressed ? s_texFormatBytesPerBlock[texture->m_dataFormat] * (left / 4) + stride * (top / 4) :
                    left * s_texFormatBitDepth[texture->m_dataFormat] / 8 + stride * top;
            }
            const uint32_t row = GxCalcTexelStrideInBytes(texture->m_format, right - left);
            const uint32_t rows = compressed ? (bottom - top + 3) / 4 : bottom - top;
            std::vector<uint8_t> pixels(static_cast<size_t>(row) * rows);
            C2iVector size{static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)};
            if (!Blit(size, BlitAlpha_0, source, stride, GxGetBlitFormat(texture->m_dataFormat),
                pixels.data(), row, GxGetBlitFormat(texture->m_format)))
                throw std::runtime_error("Unsupported Vulkan texture conversion: " +
                    std::to_string(texture->m_dataFormat) + " -> " + std::to_string(texture->m_format));
            VkBufferImageCopy copy{};
            copy.bufferOffset = Upload(pixels.data(), pixels.size(), 16);
            copy.imageSubresource = {image.aspect, level, face, 1};
            copy.imageOffset = {static_cast<int32_t>(left), static_cast<int32_t>(top), 0};
            copy.imageExtent = {right - left, bottom - top, 1};
            vkCmdCopyBufferToImage(command, upload.buffer, image.image, image.layout, 1, &copy);
        }
    }
    Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
}

namespace {
int InputLocation(MOJOSHADER_usage usage, int index) {
    if (index == 0) {
        if (usage == MOJOSHADER_USAGE_POSITION) return GxVA_Position;
        if (usage == MOJOSHADER_USAGE_BLENDWEIGHT) return GxVA_BlendWeight;
        if (usage == MOJOSHADER_USAGE_BLENDINDICES) return GxVA_BlendIndices;
        if (usage == MOJOSHADER_USAGE_NORMAL) return GxVA_Normal;
    }
    if (usage == MOJOSHADER_USAGE_COLOR && index >= 0 && index < 2) return GxVA_Color0 + index;
    if (usage == MOJOSHADER_USAGE_TEXCOORD && index >= 0 && index < 8) return GxVA_TexCoord0 + index;
    throw std::runtime_error("Unsupported Gx shader vertex semantic");
}
}

std::pair<VkBuffer, VkDeviceSize> CGxDeviceVulkan::State::Geometry(CGxBuf* buffer) {
    auto& data = pools[buffer->m_pool];
    if (!buffer->m_size || buffer->m_index > data.size() || buffer->m_size > data.size() - buffer->m_index)
        throw std::runtime_error("Geometry buffer lies outside its pool");
    if (buffer->m_pool->m_usage == GxPoolUsage_Stream)
        return {upload.buffer, Upload(data.data() + buffer->m_index, buffer->m_size)};
    auto& cached = geometry[buffer];
    if (cached.pool != buffer->m_pool || cached.index != buffer->m_index || cached.bytes != buffer->m_size) {
        cached.resident.MarkDirty();
        cached.pool = buffer->m_pool; cached.index = buffer->m_index; cached.bytes = buffer->m_size;
    }
    if (cached.resident.NeedsUpload(buffer->m_size)) EndPass();
    return {cached.resident.Resolve(device, memoryProperties, command, bufferFrame,
        data.data() + buffer->m_index, buffer->m_size, retiredBuffers, residentUploadBytes), 0};
}
void CGxDeviceVulkan::State::RetireGeometry(CGxBuf* buffer) {
    auto found = geometry.find(buffer);
    if (found != geometry.end()) {
        found->second.resident.Retire(retiredBuffers);
        geometry.erase(found);
    }
}

void CGxDeviceVulkan::State::Draw(CGxBatch& batch, bool indexed) {
    if (!batch.m_count || !BeginFrame()) return;
    gx.IRsSync(0);
    auto* vertexShader = static_cast<CGxShader*>(gx.m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);
    auto* pixelShader = static_cast<CGxShader*>(gx.m_appRenderStates[GxRs_PixelShader].m_value.m_data.p);
    const bool fixedVertexStage = !vertexShader;
    const bool fixedPixelStage = !pixelShader;
    NativeShader *v = nullptr, *p = nullptr;
    {
        auto load = [&](CGxShader* shader) -> NativeShader* {
            if (!shader->m_loaded) gx.IShaderCreate(shader);
            auto found = shaders.find(shader);
            if (!shader->m_valid || found == shaders.end()) throw std::runtime_error("Vulkan shader compilation failed; see Gx log");
            return &found->second;
        };
        if (vertexShader) v = load(vertexShader);
        if (pixelShader) p = load(pixelShader);
    }
    const auto& vertexCode = fixedVertexStage ? fixedVertex : v->code;
    const auto& pixelCode = fixedPixelStage ? fixedPixel : p->code;
    std::vector<int> required;
    if (fixedVertexStage) {
        required = {GxVA_Position, GxVA_Normal, GxVA_Color0};
        for (int i = 0; i < 8; ++i) required.push_back(GxVA_TexCoord0 + i);
    } else for (int i = 0; i < v->reflection->attribute_count; ++i)
        required.push_back(InputLocation(v->reflection->attributes[i].usage, v->reflection->attributes[i].index));
    std::array<const CGxVertexAttrib*, GxVAs_Last> declarations{};
    std::array<CGxBuf*, GxVAs_Last> buffers{};
    if (gx.m_primVertexFormat < GxVertexBufferFormats_Last) {
        const auto& description = Buffer::s_vertexBufDesc[gx.m_primVertexFormat];
        for (uint32_t i = 0; i < description.attribCount; ++i) {
            const auto& attribute = description.attribs[i];
            declarations[attribute.m_attrib] = &attribute; buffers[attribute.m_attrib] = gx.m_primVertexBuf;
        }
    } else {
        for (int i = 0; i < GxVAs_Last; ++i)
            if (gx.m_primVertexMask & (1u << i)) {
                declarations[i] = &gx.m_primVertexFormatAttrib[i]; buffers[i] = gx.m_primVertexFormatBuf[i];
            }
    }
    static constexpr VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_USCALED,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
        VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R32_SFLOAT};
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkDeviceSize> offsets;
    std::vector<VkBuffer> vertexBuffers;
    std::map<CGxBuf*, uint32_t> streams;
    for (int location : required) {
        const auto* declaration = declarations[location];
        CGxBuf* buffer = buffers[location];
        if (!declaration || !buffer) {
            if (location == GxVA_Position) throw std::runtime_error("Draw has no position buffer");
            std::array<float, 4> value{0, 0, 0, 1};
            if (location == GxVA_Color0 || location == GxVA_Color1) value = {1, 1, 1, 1};
            if (location == GxVA_Normal) value = {0, 0, 1, 0};
            const auto binding = static_cast<uint32_t>(bindings.size());
            // One instance per draw, firstInstance == 0: a constant value for
            // every vertex without a zero stride forbidden by portability_subset.
            bindings.push_back({binding, sizeof(value), VK_VERTEX_INPUT_RATE_INSTANCE});
            offsets.push_back(Upload(value.data(), sizeof(value)));
            vertexBuffers.push_back(upload.buffer);
            attributes.push_back({static_cast<uint32_t>(location), binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            continue;
        }
        if (declaration->m_type >= std::size(formats)) throw std::runtime_error("Unknown Gx vertex format");
        auto found = streams.find(buffer);
        if (found == streams.end()) {
            const auto binding = static_cast<uint32_t>(bindings.size());
            found = streams.emplace(buffer, binding).first;
            const auto source = Geometry(buffer);
            bindings.push_back({binding, buffer->m_itemSize ? buffer->m_itemSize : declaration->m_bufSize, VK_VERTEX_INPUT_RATE_VERTEX});
            offsets.push_back(source.second);
            vertexBuffers.push_back(source.first);
        }
        attributes.push_back({static_cast<uint32_t>(location), found->second, formats[declaration->m_type], declaration->m_offset});
    }
    const auto enabled = [&](EGxRenderState state, EGxMasterEnables master) {
        return gx.m_appRenderStates[state].m_value.m_data.i[0] && (gx.m_appMasterEnables & (1u << master));
    };
    const auto blend = gx.m_appRenderStates[GxRs_BlendingMode].m_value.m_data.u[0];
    if (blend >= GxBlends_Last) throw std::runtime_error("Unknown Gx blend mode");
    std::array<std::array<VkDescriptorImageInfo, 16>, 2> images{};
    // Resolve uploads before beginning a render pass, since transfers cannot
    // occur inside it. Descriptor snapshots remain valid until the frame fence.
    auto sample = [&](unsigned set, uint32_t slot, bool useBoundTexture = true) {
        if (slot >= (set == 0 ? 4u : 16u)) throw std::runtime_error("Shader texture register is out of range");
        auto& descriptor = images[set == 0 ? 0 : 1][slot];
        if (!descriptor.imageView) {
            auto* texture = useBoundTexture ? static_cast<CGxTex*>(gx.m_appRenderStates[GxRs_Texture0 + slot].m_value.m_data.p) : nullptr;
            auto& image = Texture(texture);
            descriptor = {image.sampler, image.view, image.layout};
        }
    };
    std::array<bool, 8> fixedStageEnabled{};
    if (fixedPixelStage) {
        bool enabledChain = true;
        for (unsigned stage = 0; stage < fixedStageEnabled.size(); ++stage) {
            auto* texture = static_cast<CGxTex*>(gx.m_appRenderStates[GxRs_Texture0 + stage].m_value.m_data.p);
            enabledChain = enabledChain && texture;
            fixedStageEnabled[stage] = enabledChain;
            if (enabledChain && texture->m_target == GxTex_CubeMap)
                throw std::runtime_error("Fixed-function cube-map sampling is not implemented");
            sample(2, stage, enabledChain);
        }
    } else {
        for (int i = 0; i < p->reflection->sampler_count; ++i) sample(2, p->reflection->samplers[i].index);
    }
    if (v) for (int i = 0; i < v->reflection->sampler_count; ++i) sample(0, v->reflection->samplers[i].index);
    C44Matrix transform = gx.m_xforms[GxXform_World].m_mtx[gx.m_xforms[GxXform_World].m_level] *
        gx.m_xforms[GxXform_View].m_mtx[gx.m_xforms[GxXform_View].m_level] * gx.m_projNative;
    std::vector<float> vertexUniforms, pixelUniforms;
    if (fixedVertexStage || fixedPixelStage) {
        const auto& world = gx.m_xforms[GxXform_World].m_mtx[gx.m_xforms[GxXform_World].m_level];
        const auto& view = gx.m_xforms[GxXform_View].m_mtx[gx.m_xforms[GxXform_View].m_level];
        const C44Matrix worldView = world * view;
        C44Matrix normalTransform;
        for (unsigned stage = 0; stage < 8; ++stage) {
            const auto generation = gx.m_appRenderStates[GxRs_TexGen0 + stage].m_value.m_data.u[0];
            if (fixedVertexStage && (fixedStageEnabled[stage] || !fixedPixelStage) && generation >= GxTexGen_ReflectionMap)
                normalTransform = worldView.Inverse().Transpose();
        }
        const auto appendMatrix = [&](const C44Matrix& matrix) {
            const auto* data = reinterpret_cast<const float*>(&matrix);
            vertexUniforms.insert(vertexUniforms.end(), data, data + 16);
        };
        appendMatrix(transform); appendMatrix(worldView); appendMatrix(normalTransform);
        std::array<float, 32> coordinates{};
        pixelUniforms = {gx.m_appRenderStates[GxRs_AlphaRef].m_value.m_data.u[0] / 255.0f, 0, 0, 0};
        for (unsigned stage = 0; stage < 8; ++stage) {
            C44Matrix texGen;
            const bool enabledStage = fixedStageEnabled[stage] || !fixedPixelStage;
            const auto coordinate = gx.m_appRenderStates[GxRs_TextureCoord0 + stage].m_value.m_data.u[0];
            const auto generation = gx.m_appRenderStates[GxRs_TexGen0 + stage].m_value.m_data.u[0];
            const auto transformMode = gx.m_appRenderStates[GxRs_TextureShader0 + stage].m_value.m_data.u[0];
            const auto colourOp = gx.m_appRenderStates[GxRs_ColorOp0 + stage].m_value.m_data.u[0];
            const auto alphaOp = gx.m_appRenderStates[GxRs_AlphaOp0 + stage].m_value.m_data.u[0];
            if ((fixedVertexStage && enabledStage && (coordinate >= 8 || generation > GxTexGen_SphereMap || transformMode > 2)) ||
                (fixedPixelStage && fixedStageEnabled[stage] && (colourOp >= 6 || alphaOp >= 6)))
                throw std::runtime_error("Unsupported fixed-function texture-stage state");
            if (enabledStage && (generation == GxTexGen_ObjectLinear || generation == GxTexGen_EyeLinear)) {
                texGen = view.AffineInverse();
                if (generation == GxTexGen_ObjectLinear) texGen = texGen * world.Inverse();
            } else if (enabledStage && generation == GxTexGen_SphereMap) {
                texGen.a0 = texGen.b1 = 0.5f; texGen.d0 = texGen.d1 = 0.5f;
            }
            if (enabledStage && transformMode)
                texGen = texGen * gx.m_xforms[stage].m_mtx[gx.m_xforms[stage].m_level];
            appendMatrix(texGen);
            coordinates[stage * 4] = static_cast<float>(coordinate);
            coordinates[stage * 4 + 1] = static_cast<float>(generation);
            coordinates[stage * 4 + 2] = static_cast<float>(transformMode);
            coordinates[stage * 4 + 3] = enabledStage ? 1.0f : 0.0f;
            pixelUniforms.insert(pixelUniforms.end(), {fixedStageEnabled[stage] ? 1.0f : 0.0f,
                static_cast<float>(colourOp), static_cast<float>(alphaOp), 0});
        }
        vertexUniforms.insert(vertexUniforms.end(), coordinates.begin(), coordinates.end());
    }
    if (v) vertexUniforms = GxVulkan::ShaderUniforms(*v->reflection, CGxDevice::s_shadowConstants[1].constants, 256);
    if (p) pixelUniforms = GxVulkan::ShaderUniforms(*p->reflection, CGxDevice::s_shadowConstants[0].constants, 256);
    if (vertexUniforms.empty()) vertexUniforms.resize(4);
    if (pixelUniforms.empty()) pixelUniforms.resize(4);
    const size_t alignment = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 16);
    VkDescriptorBufferInfo uniforms[] = {
        {upload.buffer, Upload(vertexUniforms.data(), vertexUniforms.size() * 4, alignment), vertexUniforms.size() * 4},
        {upload.buffer, Upload(pixelUniforms.data(), pixelUniforms.size() * 4, alignment), pixelUniforms.size() * 4},
        {upload.buffer, 0, 16}
    };
    const float alphaTest[] = {gx.m_appRenderStates[GxRs_AlphaRef].m_value.m_data.u[0] / 255.0f, 0, 0, 0};
    uniforms[2].offset = Upload(alphaTest, sizeof(alphaTest), alignment);
    std::array<VkDescriptorSet, 4> sets;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptorPool; allocate.descriptorSetCount = 4; allocate.pSetLayouts = descriptorLayouts.data();
    GX_VK(vkAllocateDescriptorSets(device, &allocate, sets.data()));
    std::vector<VkWriteDescriptorSet> writes;
    for (unsigned stage = 0; stage < 2; ++stage) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[stage * 2 + 1]; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1; write.pBufferInfo = &uniforms[stage]; writes.push_back(write);
    }
    VkWriteDescriptorSet alpha{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    alpha.dstSet = sets[3]; alpha.dstBinding = 1; alpha.descriptorCount = 1;
    alpha.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; alpha.pBufferInfo = &uniforms[2]; writes.push_back(alpha);
    const auto writeSampler = [&](unsigned set, uint32_t binding) {
        if (binding >= (set == 0 ? 4u : 16u)) throw std::runtime_error("Shader sampler exceeds its stage limit");
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[set]; write.dstBinding = binding; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &images[set == 0 ? 0 : 1][binding]; writes.push_back(write);
    };
    if (v) for (int i = 0; i < v->reflection->sampler_count; ++i) writeSampler(0, v->reflection->samplers[i].index);
    if (fixedPixelStage) for (unsigned stage = 0; stage < 8; ++stage) writeSampler(2, stage);
    else for (int i = 0; i < p->reflection->sampler_count; ++i) writeSampler(2, p->reflection->samplers[i].index);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    std::pair<VkBuffer, VkDeviceSize> residentIndex{};
    if (indexed && batch.m_primType != GxPrim_TriangleFan) {
        auto* buffer = gx.m_primIndexBuf;
        if (!buffer || !buffer->m_pool) throw std::runtime_error("Indexed draw has no index buffer");
        if (buffer->m_pool->m_usage != GxPoolUsage_Stream) residentIndex = Geometry(buffer);
    }
    BeginPass();
    const uint32_t depthTest = enabled(GxRs_DepthTest, GxMasterEnable_DepthTest) ? 1u : 0u;
    const uint32_t depthWrite = enabled(GxRs_DepthWrite, GxMasterEnable_DepthWrite) ? 1u : 0u;
    const uint32_t cull = enabled(GxRs_Culling, GxMasterEnable_Culling) ? gx.m_appRenderStates[GxRs_Culling].m_value.m_data.u[0] : 0;
    const uint32_t colourMask = enabled(GxRs_ColorWrite, GxMasterEnable_ColorWrite) ? gx.m_appRenderStates[GxRs_ColorWrite].m_value.m_data.u[0] : 0;
    const auto depthFunction = gx.m_appRenderStates[GxRs_DepthFunc].m_value.m_data.u[0];
    const auto primitive = batch.m_primType == GxPrim_TriangleFan ? GxPrim_Triangles : batch.m_primType;
    std::vector<uint64_t> key{fixedVertexStage ? 0 : v->id, fixedPixelStage ? 0 : p->id, reinterpret_cast<uint64_t>(currentPass),
        blend, depthTest, depthWrite, cull, colourMask, depthFunction, static_cast<uint64_t>(primitive)};
    for (const auto& binding : bindings) {
        key.push_back(binding.binding); key.push_back(binding.stride); key.push_back(binding.inputRate);
    }
    key.push_back(UINT64_MAX);
    for (const auto& attribute : attributes) {
        key.push_back(attribute.location); key.push_back(attribute.binding); key.push_back(attribute.format); key.push_back(attribute.offset);
    }
    auto found = pipelines.find(key);
    if (found == pipelines.end()) {
        VkShaderModule modules[2]{};
        try {
            const std::vector<uint32_t>* codes[] = {&vertexCode, &pixelCode};
            for (unsigned i = 0; i < 2; ++i) {
                VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                module.codeSize = codes[i]->size() * 4; module.pCode = codes[i]->data();
                GX_VK(vkCreateShaderModule(device, &module, nullptr, &modules[i]));
            }
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (unsigned i = 0; i < 2; ++i) {
                stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
                stages[i].module = modules[i]; stages[i].pName = "main";
            }
            VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            inputs.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size()); inputs.pVertexBindingDescriptions = bindings.data();
            inputs.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()); inputs.pVertexAttributeDescriptions = attributes.data();
            static constexpr VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
            if (primitive >= GxPrims_Last) throw std::runtime_error("Unknown Gx primitive type");
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = topologies[primitive];
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1.0f;
            raster.cullMode = cull == 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            raster.frontFace = cull == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            static constexpr VkCompareOp comparisons[] = {VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS};
            if (depthFunction >= std::size(comparisons)) throw std::runtime_error("Unknown Gx depth comparison");
            VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthState.depthTestEnable = depthTest; depthState.depthWriteEnable = depthWrite;
            depthState.depthCompareOp = comparisons[depthFunction];
            static constexpr VkBlendFactor src[] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA,
                VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_ALPHA};
            static constexpr VkBlendFactor dst[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.blendEnable = blend > GxBlend_AlphaKey;
            attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = src[blend];
            attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = dst[blend];
            attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            attachment.colorWriteMask = colourMask & 15;
            VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blending.attachmentCount = 1; blending.pAttachments = &attachment;
            const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 3; dynamic.pDynamicStates = dynamicStates;
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            create.stageCount = 2; create.pStages = stages; create.pVertexInputState = &inputs; create.pInputAssemblyState = &assembly;
            create.pViewportState = &viewport; create.pRasterizationState = &raster; create.pMultisampleState = &samples;
            create.pDepthStencilState = &depthState; create.pColorBlendState = &blending; create.pDynamicState = &dynamic;
            create.layout = pipelineLayout; create.renderPass = currentPass;
            Pipeline pipeline;
            GX_VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline.handle));
            found = pipelines.emplace(std::move(key), pipeline).first;
        } catch (...) {
            for (auto module : modules) if (module) vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
        for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, found->second.handle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 4, sets.data(), 0, nullptr);
    vkCmdBindVertexBuffers(command, 0, static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data(), offsets.data());
    const bool textureTarget = gx.m_textureTarget[GxBuffers_Color].m_texture != nullptr;
    const auto viewport = GxVulkan::MakeViewport(targetExtent, textureTarget,
        gx.m_viewport.m_x.m_l, gx.m_viewport.m_x.m_h,
        gx.m_viewport.m_y.m_l, gx.m_viewport.m_y.m_h, gx.m_viewport.m_z.m_l, gx.m_viewport.m_z.m_h);
    vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, targetExtent};
    if (gx.m_appRenderStates[GxRs_ScissorTest].m_value.m_data.i[0]) {
        scissor = GxVulkan::MakeScissor(targetExtent, textureTarget, gx.m_scissorRect.minX,
            gx.m_scissorRect.maxX, gx.m_scissorRect.minY, gx.m_scissorRect.maxY);
    }
    vkCmdSetScissor(command, 0, 1, &scissor);
    const float blendConstants[] = {1, 1, 1, 1};
    vkCmdSetBlendConstants(command, blendConstants);
    VkDeviceSize indexOffset = 0;
    if (batch.m_primType == GxPrim_TriangleFan) {
        if (batch.m_count < 3) return;
        std::vector<uint32_t> indices;
        const uint16_t* original = nullptr;
        if (indexed) {
            auto* buffer = gx.m_primIndexBuf;
            if (!buffer) throw std::runtime_error("Indexed draw has no index buffer");
            auto& data = pools[buffer->m_pool];
            if (buffer->m_index > data.size() || (static_cast<uint64_t>(batch.m_start) + batch.m_count) * 2 > data.size() - buffer->m_index)
                throw std::runtime_error("Triangle fan index buffer overrun");
            original = reinterpret_cast<const uint16_t*>(data.data() + buffer->m_index) + batch.m_start;
        }
        for (uint32_t i = 1; i + 1 < batch.m_count; ++i) {
            indices.push_back(original ? original[0] : batch.m_start);
            indices.push_back(original ? original[i] : batch.m_start + i);
            indices.push_back(original ? original[i + 1] : batch.m_start + i + 1);
        }
        indexOffset = Upload(indices.data(), indices.size() * 4, 4);
        vkCmdBindIndexBuffer(command, upload.buffer, indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    } else if (indexed) {
        auto* buffer = gx.m_primIndexBuf;
        if (!buffer) throw std::runtime_error("Indexed draw has no index buffer");
        auto& data = pools[buffer->m_pool];
        if (buffer->m_index > data.size() || (static_cast<uint64_t>(batch.m_start) + batch.m_count) * 2 > data.size() - buffer->m_index)
            throw std::runtime_error("Index buffer overrun");
        VkBuffer indexBuffer = upload.buffer;
        if (residentIndex.first) {
            indexBuffer = residentIndex.first;
            indexOffset = residentIndex.second + static_cast<uint64_t>(batch.m_start) * 2;
        } else indexOffset = Upload(data.data() + buffer->m_index + batch.m_start * 2, batch.m_count * 2, 4);
        vkCmdBindIndexBuffer(command, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(command, batch.m_count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(command, batch.m_count, 1, batch.m_start, 0);
    }
}

void CGxDeviceVulkan::State::Submit(bool present, bool resume) {
    EndPass();
    if (present) Barrier(swapImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    GX_VK(vkEndCommandBuffer(command));
    GX_VK(vkResetFences(device, 1, &fence));
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (acquirePending) {
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
    }
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    if (present) { submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &complete[imageIndex]; }
    GX_VK(vkQueueSubmit(queue, 1, &submit, fence));
    recording = false; acquirePending = false;
    if (present) {
        VkPresentInfoKHR presentation{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentation.waitSemaphoreCount = 1; presentation.pWaitSemaphores = &complete[imageIndex];
        presentation.swapchainCount = 1; presentation.pSwapchains = &swapchain; presentation.pImageIndices = &imageIndex;
        const auto result = vkQueuePresentKHR(queue, &presentation);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) resize = true;
        else VkCheck(result, "vkQueuePresentKHR");
    }
    if (resume) {
        GX_VK(vkWaitForFences(device, 1, &fence, true, UINT64_MAX));
        GX_VK(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GX_VK(vkBeginCommandBuffer(command, &begin));
        recording = true;
    }
}

CGxDeviceVulkan::CGxDeviceVulkan() : m_native(std::make_unique<State>(*this)) {
    m_api = GxApi_Vulkan;
    m_caps.m_colorFormat = GxCF_argb;
    DeviceCreatePools();
    DeviceCreateStreamBufs();
}
CGxDeviceVulkan::~CGxDeviceVulkan() { DeviceDestroy(); }
CGxDevice* CreateCGxDeviceVulkan() { return new CGxDeviceVulkan; }
int32_t CGxDeviceVulkan::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    m_windowProc = windowProc;
    m_native->ownWindow = true;
    return DeviceSetFormat(format);
}
int32_t CGxDeviceVulkan::DeviceCreate(void* window, const CGxFormat& format) {
    if (!window) return 0;
    m_native->window = static_cast<SDL_Window*>(window);
    m_native->ownWindow = false;
    return DeviceSetFormat(format);
}
int32_t CGxDeviceVulkan::DeviceSetFormat(const CGxFormat& format) {
    auto& state = *m_native;
    if (format.m_size.x < 0 || format.m_size.y < 0) return 0;
    try {
        CGxDevice::DeviceSetFormat(format);
        if (state.device) {
            SDL_SetWindowSize(state.window, m_format.m_size.x, m_format.m_size.y);
            const uint32_t fullscreen = m_format.m_window ? (m_format.m_maximize == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) : SDL_WINDOW_FULLSCREEN;
            if (SDL_SetWindowFullscreen(state.window, fullscreen)) throw std::runtime_error(SDL_GetError());
            state.resize = true;
            return 1;
        }
        if (!state.window) {
            if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) throw std::runtime_error("SDL video is not initialized");
            uint32_t flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
            if (!m_format.m_window) flags |= SDL_WINDOW_FULLSCREEN;
            else if (m_format.m_maximize == 1) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            state.window = SDL_CreateWindow("World of Warcraft (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                m_format.m_size.x ? m_format.m_size.x : 1024, m_format.m_size.y ? m_format.m_size.y : 768, flags);
            if (!state.window) throw std::runtime_error(SDL_GetError());
        }
        OsGuiSetGxWindow(state.window);
        state.Initialize();
        m_caps.m_numTmus = std::min(state.properties.limits.maxPerStageDescriptorSamplers, 16u);
        m_caps.m_numStreams = std::min(state.properties.limits.maxVertexInputBindings, 14u);
        m_caps.m_streamOffset = 1; m_caps.m_maxIndex = 65535;
        m_caps.m_pixelCenterOnEdge = m_caps.m_texelCenterOnEdge = 1;
        m_caps.m_texNonPow2 = 1;
        for (unsigned i = 0; i < GxTexTargets_Last; ++i) {
            m_caps.m_texTarget[i] = i != GxTex_Rectangle; // Rectangle coordinates are unnormalised.
            m_caps.m_texMaxSize[i] = i == GxTex_CubeMap ? state.properties.limits.maxImageDimensionCube : state.properties.limits.maxImageDimension2D;
        }
        for (unsigned i = 1; i < GxTexFormats_Last; ++i) {
            VkFormatProperties properties;
            vkGetPhysicalDeviceFormatProperties(state.physical, TextureFormat(static_cast<EGxTexFormat>(i)), &properties);
            m_caps.m_texFmt[i] = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
            m_caps.m_rttFormat[i] = (properties.optimalTilingFeatures & (i == GxTex_D24X8 ?
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) != 0;
        }
        m_caps.m_shaderTargets[GxSh_Vertex] = 3; m_caps.m_shaderTargets[GxSh_Pixel] = 4;
        m_caps.m_shaderConstants[GxSh_Vertex] = 256; m_caps.m_shaderConstants[GxSh_Pixel] = 224;
        m_caps.m_texFilterTrilinear = 1; m_caps.m_colorWrite = 1;
        m_caps.m_blendFactor = 1; m_caps.m_shaderAlphaRef = 0;
        m_caps.m_hwCursor = 0; m_caps.m_occlusionQuery = 0;
        m_context = m_windowVisible = 1;
        m_windowFocus = (SDL_GetWindowFlags(state.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        ICursorCreate(m_format);
        SDL_ShowCursor(SDL_DISABLE);
        OsGuiSetNativeWindowEventProc(WindowEvent, this);
        SDL_PumpEvents();
        DeviceWM(GxWM_Size, 0, 0);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native Vulkan initialization: %s\n", error.what());
        CGxDevice::Log("Native Vulkan initialization: %s", error.what());
        DeviceDestroy();
        return 0;
    }
}
void CGxDeviceVulkan::DeviceDestroy() {
    if (!m_native) return;
    if (OsGuiGetWindow(0) == m_native->window) OsGuiSetNativeWindowEventProc(nullptr, nullptr);
    ICursorDestroy();
    m_context = m_windowVisible = m_windowFocus = 0;
    m_native->Destroy();
    CGxDevice::DeviceDestroy();
    SDL_ShowCursor(SDL_ENABLE);
}
void* CGxDeviceVulkan::DeviceWindow() { return m_native->window; }
int32_t CGxDeviceVulkan::DeviceWM(EGxWM wm, uintptr_t, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size: {
            m_native->resize = true;
            m_windowVisible = param2 != 1 && param2 != 2;
            if (m_windowVisible) {
                int width, height;
                SDL_GetWindowSize(m_native->window, &width, &height);
                if (width > 0 && height > 0) {
                    CRect rect;
                    rect.minX = rect.minY = 0;
                    rect.maxX = static_cast<float>(width); rect.maxY = static_cast<float>(height);
                    // Size callbacks measure fonts before the deferred swapchain
                    // recreation. Publish the new dimensions synchronously.
                    DeviceSetDefWindow(rect);
                    m_viewportDirty = 1;
                }
            }
            return 1;
        }
        case GxWM_SetFocus: m_windowFocus = 1; return 1;
        case GxWM_KillFocus: m_windowFocus = 0; return 1;
        case GxWM_DisplayChange: m_native->resize = true; return 1;
        case GxWM_Destroy: DeviceDestroy(); return 1;
    }
    return 0;
}
void CGxDeviceVulkan::CapsWindowSize(CRect& rect) { rect = DeviceCurWindow(); }
void CGxDeviceVulkan::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = DeviceCurWindow();
    int x = 0, y = 0;
    SDL_GetWindowPosition(m_native->window, &x, &y);
    rect.minX += x; rect.maxX += x; rect.minY += y; rect.maxY += y;
}
void CGxDeviceVulkan::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);
    // Convert GL's [-w,w] clip depth to Vulkan's [0,w], keeping the engine's
    // matrix convention. A negative viewport height handles the Y convention.
    m_projNative = matrix;
    m_projNative.a2 = (matrix.a2 + matrix.a3) * 0.5f;
    m_projNative.b2 = (matrix.b2 + matrix.b3) * 0.5f;
    m_projNative.c2 = (matrix.c2 + matrix.c3) * 0.5f;
    m_projNative.d2 = (matrix.d2 + matrix.d3) * 0.5f;
}
void CGxDeviceVulkan::SceneClear(uint32_t mask, CImVector colour) {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) return;
    state.BeginPass();
    std::vector<VkClearAttachment> attachments;
    if (mask & 1) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color = {{colour.r / 255.0f, colour.g / 255.0f, colour.b / 255.0f, colour.a / 255.0f}};
        attachments.push_back(clear);
    }
    if (mask & 2) {
        VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil.depth = 1.0f;
        attachments.push_back(clear);
    }
    VkClearRect rect{}; rect.rect.extent = state.targetExtent; rect.layerCount = 1;
    if (!attachments.empty()) vkCmdClearAttachments(state.command, static_cast<uint32_t>(attachments.size()), attachments.data(), 1, &rect);
}
void CGxDeviceVulkan::ScenePresent() {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) return;
    ICursorDraw();
    if (m_scrShotClick) DeviceScreenShot();
    WaitForFPSCap();
    state.Submit(true, false);
    CGxDevice::ScenePresent();
}
void CGxDeviceVulkan::Draw(CGxBatch* batch, int32_t indexed) {
    if (m_context && batch) m_native->Draw(*batch, indexed != 0);
}
void CGxDeviceVulkan::IRsSendToHw(EGxRenderState) {
    // Gx still tracks dirty state, but Vulkan snapshots it into each draw's
    // pipeline key/descriptors rather than emitting mutable driver state here.
}
void CGxDeviceVulkan::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (!pool || size > INT32_MAX) throw std::invalid_argument("Invalid Gx pool size");
    m_native->pools[pool].resize(size); pool->m_size = static_cast<int32_t>(size);
    for (auto& entry : m_native->geometry)
        if (entry.second.pool == pool) entry.second.resident.MarkDirty();
}
void CGxDeviceVulkan::PoolDestroy(CGxPool* pool) {
    if (pool) {
        for (auto entry = m_native->geometry.begin(); entry != m_native->geometry.end();) {
            if (entry->second.pool == pool) {
                entry->second.resident.Retire(m_native->retiredBuffers);
                entry = m_native->geometry.erase(entry);
            } else ++entry;
        }
        m_native->pools.erase(pool);
    }
    CGxDevice::PoolDestroy(pool);
}
void CGxDeviceVulkan::BufDestroy(CGxBuf*& buffer) {
    if (buffer) m_native->RetireGeometry(buffer);
    CGxDevice::BufDestroy(buffer);
}
char* CGxDeviceVulkan::BufLock(CGxBuf* buffer) {
    if (!buffer || !buffer->m_pool) throw std::invalid_argument("Invalid Gx buffer lock");
    CGxDevice::BufLock(buffer);
    auto* pool = buffer->m_pool;
    if (pool->m_usage == GxPoolUsage_Stream) {
        if (!buffer->m_itemSize) throw std::runtime_error("Stream buffer has no item stride");
        uint32_t offset = (static_cast<uint32_t>(pool->m_offset) + buffer->m_itemSize - 1) / buffer->m_itemSize * buffer->m_itemSize;
        if (offset > static_cast<uint32_t>(std::max(pool->m_size, 0)) || buffer->m_size > static_cast<uint32_t>(std::max(pool->m_size, 0)) - offset) {
            pool->Discard(); offset = 0;
        }
        buffer->m_index = offset;
        pool->m_offset = static_cast<int32_t>(offset + buffer->m_size);
    }
    auto& data = m_native->pools[pool];
    const uint64_t required = static_cast<uint64_t>(buffer->m_index) + buffer->m_size;
    if (required > INT32_MAX) throw std::runtime_error("Gx buffer size overflow");
    if (data.size() < required) data.resize(std::max<uint64_t>(required, std::max(pool->m_size, 0)));
    for (auto& entry : m_native->geometry) {
        const auto& cached = entry.second;
        if (cached.pool == pool && cached.index < required &&
            buffer->m_index < static_cast<uint64_t>(cached.index) + cached.bytes)
            entry.second.resident.MarkDirty();
    }
    return reinterpret_cast<char*>(data.data() + buffer->m_index);
}
int32_t CGxDeviceVulkan::BufUnlock(CGxBuf* buffer, uint32_t size) {
    if (!buffer || size > buffer->m_size) throw std::invalid_argument("Invalid Gx buffer unlock size");
    buffer->m_valid = buffer->m_hasData = 1;
    return CGxDevice::BufUnlock(buffer, size);
}
void CGxDeviceVulkan::BufData(CGxBuf* buffer, const void* data, uint32_t size, uint32_t offset) {
    if (!buffer || (size && !data) || offset > buffer->m_size || size > buffer->m_size - offset)
        throw std::invalid_argument("Invalid Gx buffer update");
    char* destination = BufLock(buffer);
    if (size) std::memcpy(destination + offset, data, size);
    BufUnlock(buffer, buffer->m_size);
}
int32_t CGxDeviceVulkan::TexCreate(EGxTexTarget target, uint32_t width, uint32_t height, uint32_t depth,
    EGxTexFormat format, EGxTexFormat dataFormat, CGxTexFlags flags, void* userArg,
    GxTexUserFunc userFunc, const char* name, CGxTex*& texture) {
    if (target < GxTex_2d || target >= GxTexTargets_Last || !m_caps.m_texTarget[target]) return 0;
    if (format <= GxTex_Unknown || format >= GxTexFormats_Last) return 0;
    // Packed ARGB4444 has no mandatory Vulkan equivalent. Keep its source
    // format and use the existing Blit conversion into an advertised format.
    if (format == GxTex_Argb4444) format = GxTex_Abgr8888;
    else if (!m_caps.m_texFmt[format] && format != GxTex_D24X8) format = GxTex_Argb8888;
    return CGxDevice::TexCreate(target, width, height, depth, format, dataFormat, flags, userArg, userFunc, name, texture);
}
void CGxDeviceVulkan::ITexMarkAsUpdated(CGxTex* texture) {
    if (m_context && texture && m_native->BeginFrame()) m_native->Texture(texture);
}
void CGxDeviceVulkan::TexDestroy(CGxTex* texture) {
    auto found = m_native->textures.find(texture);
    if (found != m_native->textures.end()) {
        m_native->EndPass();
        m_native->retiredImages.push_back(found->second); m_native->textures.erase(found);
    }
    texture->m_apiSpecificData = nullptr;
    CGxDevice::TexDestroy(texture);
}
void CGxDeviceVulkan::IShaderCreate(CGxShader* shader) {
    shader->m_loaded = 1; shader->m_valid = 0;
    try {
        auto recovered = GxVulkan::RecoverSlangShader(shader->m_code.Ptr(), shader->m_code.Count());
        auto code = m_native->shaderCompiler.Compile(recovered.source, shader->m_target == GxSh_Vertex);
        m_native->shaders.insert_or_assign(shader, NativeShader{m_native->nextShader++, std::move(recovered.reflection), std::move(code)});
        shader->m_apiSpecific = &m_native->shaders.at(shader); shader->m_valid = 1;
    } catch (const std::exception& error) {
        CGxDevice::Log("Vulkan/Slang shader: %s", error.what());
        std::fprintf(stderr, "Vulkan/Slang shader: %s\n", error.what());
    }
}
void CGxDeviceVulkan::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    m_native->shaders.erase(shader); shader->m_apiSpecific = nullptr;
    IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    IShaderCreate(shader);
}
void CGxDeviceVulkan::ShaderDestroy(CGxShader*& shader) {
    if (shader && shader->m_refCount == 1) {
        m_native->shaders.erase(shader); shader->m_apiSpecific = nullptr;
    }
    CGxDevice::ShaderDestroy(shader);
}

void CGxDeviceVulkan::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (buffer >= GxBuffers_Last) throw std::invalid_argument("Invalid Gx render target");
    m_native->EndPass();
    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
}
int32_t CGxDeviceVulkan::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos,
    const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!dest || size.x <= 0 || size.y <= 0) return 0;
    CiRect destination, source;
    destination.minX = destPos.x; destination.minY = destPos.y;
    destination.maxX = destPos.x + size.x; destination.maxY = destPos.y + size.y;
    source.minX = sourcePos.x; source.minY = sourcePos.y;
    source.maxX = sourcePos.x + size.x; source.maxY = sourcePos.y + size.y;
    return TexStretch(dest, &destination, &source, level, plane);
}
int32_t CGxDeviceVulkan::TexStretch(CGxTex* dest, const CiRect* destRect,
    const CiRect* sourceRect, uint32_t level, uint32_t face) {
    auto& state = *m_native;
    if (!dest || !m_context || !state.BeginFrame()) return 0;
    state.EndPass();
    auto& destination = state.Texture(dest);
    auto* sourceTarget = m_textureTarget[GxBuffers_Color].m_texture;
    auto* sourceImage = sourceTarget ? &state.Texture(sourceTarget) : nullptr;
    const VkImage source = sourceImage ? sourceImage->image : state.swapImages[state.imageIndex];
    const auto width = sourceImage ? sourceImage->width : state.extent.width;
    const auto height = sourceImage ? sourceImage->height : state.extent.height;
    if (destination.aspect != VK_IMAGE_ASPECT_COLOR_BIT || level >= destination.levels || face >= destination.layers)
        return 0;
    if (source == destination.image) throw std::runtime_error("Overlapping render-target blit");
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
    blit.srcOffsets[0] = {sourceRect ? sourceRect->minX : 0, sourceRect ? sourceRect->minY : 0, 0};
    blit.srcOffsets[1] = {sourceRect ? sourceRect->maxX : static_cast<int32_t>(width), sourceRect ? sourceRect->maxY : static_cast<int32_t>(height), 1};
    blit.dstOffsets[0] = {destRect ? destRect->minX : 0, destRect ? destRect->minY : 0, 0};
    blit.dstOffsets[1] = {destRect ? destRect->maxX : static_cast<int32_t>(std::max(destination.width >> level, 1u)),
        destRect ? destRect->maxY : static_cast<int32_t>(std::max(destination.height >> level, 1u)), 1};
    state.Barrier(source, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    state.Barrier(destination.image, destination.aspect, destination.levels, destination.layers, destination.layout, destination.layout);
    vkCmdBlitImage(state.command, source, VK_IMAGE_LAYOUT_GENERAL, destination.image, destination.layout, 1, &blit, VK_FILTER_NEAREST);
    state.Barrier(destination.image, destination.aspect, destination.levels, destination.layers, destination.layout, destination.layout);
    return 1;
}
namespace {
void ReadbackBarrier(VkCommandBuffer command, VkBuffer buffer) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
}
}
void CGxDeviceVulkan::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) { pixels.SetCount(0); return; }
    ClampRectToWindow(rect);
    const uint32_t width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    if (!width || !height) { pixels.SetCount(0); return; }
    state.EndPass();
    auto* target = m_textureTarget[GxBuffers_Color].m_texture;
    auto* image = target ? &state.Texture(target) : nullptr;
    const auto source = image ? image->image : state.swapImages[state.imageIndex];
    const auto format = image ? image->format : state.surfaceFormat;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM)
        throw std::runtime_error("Unsupported colour readback format");
    NativeBuffer readback = state.MakeBuffer(static_cast<uint64_t>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        state.Barrier(source, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {rect.minX, rect.minY, 0}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(state.command, source, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        ReadbackBarrier(state.command, readback.buffer);
        state.Submit(false, true);
        pixels.SetCount(width * height);
        std::memcpy(pixels.Ptr(), readback.mapped, static_cast<size_t>(width) * height * 4);
        if (format == VK_FORMAT_R8G8B8A8_UNORM)
            for (uint32_t i = 0; i < width * height; ++i) std::swap(pixels[i].r, pixels[i].b);
    } catch (...) { state.DestroyBuffer(readback); throw; }
    state.DestroyBuffer(readback);
}
void CGxDeviceVulkan::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    auto& state = *m_native;
    if (!m_context || !state.BeginFrame()) { depths.SetCount(0); return; }
    ClampRectToWindow(rect);
    const uint32_t width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    if (!width || !height) { depths.SetCount(0); return; }
    state.EndPass();
    auto* target = m_textureTarget[GxBuffers_Depth].m_texture;
    auto& image = target ? state.Texture(target) : state.depth;
    if (image.format != VK_FORMAT_D32_SFLOAT) throw std::runtime_error("Unsupported depth readback format");
    NativeBuffer readback = state.MakeBuffer(static_cast<uint64_t>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        state.Barrier(image.image, image.aspect, image.levels, image.layers, image.layout, image.layout);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.imageOffset = {rect.minX, rect.minY, 0}; copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(state.command, image.image, image.layout, readback.buffer, 1, &copy);
        ReadbackBarrier(state.command, readback.buffer);
        state.Submit(false, true);
        depths.SetCount(width * height);
        std::memcpy(depths.Ptr(), readback.mapped, static_cast<size_t>(width) * height * 4);
    } catch (...) { state.DestroyBuffer(readback); throw; }
    state.DestroyBuffer(readback);
}
// These capabilities are deliberately not advertised. Return unsupported rather
// than synthesising occlusion results or pretending vendor stereo is available.
bool CGxDeviceVulkan::QueryBegin(CGxQuery*) { return false; }
bool CGxDeviceVulkan::QueryEnd(CGxQuery*) { return false; }
bool CGxDeviceVulkan::QueryGetParam(CGxQuery*, EGxQueryParam, uint32_t&) { return false; }
bool CGxDeviceVulkan::QueryGetData(CGxQuery*, uint32_t&) { return false; }
void CGxDeviceVulkan::StereoSetConvergence(float) {}
float CGxDeviceVulkan::StereoGetConvergence() { return 0.0f; }
void CGxDeviceVulkan::StereoSetSeparation(float) {}
float CGxDeviceVulkan::StereoGetSeparation() { return 0.0f; }
bool CGxDeviceVulkan::StereoEnabled() { return false; }
// These three hooks are also empty in the reconstructed D3D9 backend.
void CGxDeviceVulkan::DeviceUnk81(uint32_t, uint32_t) {}
void CGxDeviceVulkan::DeviceUnk82() {}
void CGxDeviceVulkan::DeviceUnk83(uint32_t, uint32_t) {}

int32_t VulkanAdapterFormats(TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    const int count = SDL_GetNumDisplayModes(0);
    for (int i = 0; i < count; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(0, i, &mode) != 0 || mode.w < 640 || mode.h < 480) continue;
        CGxFormat format;
        format.m_size = {mode.w, mode.h}; format.m_refreshRate = mode.refresh_rate;
        format.m_colorFormat = CGxFormat::Fmt_Argb8888; format.m_depthFormat = CGxFormat::Fmt_Ds320;
        format.m_multisampleCount = 1;
        if (!CGxDevice::FormatExists(formats, format)) *formats.New() = format;
    }
    return formats.Count() != 0;
}
int32_t VulkanAdapterID(uint16_t& vendor, uint16_t& deviceID, uint32_t& driverHi, uint32_t& driverLo) {
    VkInstance instance = VK_NULL_HANDLE;
    try {
        uint32_t count = 0;
        GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        GX_VK(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()));
        const bool portability = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        });
        const char* portabilityName = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        if (portability) {
            create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            create.enabledExtensionCount = 1; create.ppEnabledExtensionNames = &portabilityName;
        }
        GX_VK(vkCreateInstance(&create, nullptr, &instance));
        GX_VK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        GX_VK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[0], &properties);
        vendor = static_cast<uint16_t>(properties.vendorID); deviceID = static_cast<uint16_t>(properties.deviceID);
        driverHi = 0; driverLo = properties.driverVersion;
        vkDestroyInstance(instance, nullptr);
        return 1;
    } catch (const std::exception& error) {
        if (instance) vkDestroyInstance(instance, nullptr);
        std::fprintf(stderr, "Vulkan adapter query: %s\n", error.what());
        return 0;
    }
}
