#include <vulkan/vulkan.h>
#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Slang.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/ResidentBuffer.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/GpuAdmission.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/WorldTarget.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/TemporalBackend.h"
#include <limits>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Vk(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Vulkan fixture error " + std::to_string(result)); }
using Matrix = std::array<float, 16>;
Matrix Identity() { Matrix m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
struct VertexConstants { Matrix projection, view, normal; std::array<Matrix, 8> texture; std::array<std::array<float, 4>, 8> control; std::array<float,4> fog; };
struct PixelConstants { std::array<float, 4> options; std::array<std::array<float, 4>, 8> stage; std::array<float,4> fog; };
static_assert(sizeof(VertexConstants) == 848 && sizeof(PixelConstants) == 160, "Fixed shader uniform ABI changed");
struct Vertex { float position[3], normal[3]; uint32_t colour; float uv[8][2]; };
struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void* mapped{}; };
struct Image { VkImage handle{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct Fixture {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkCommandPool commandPool{}; VkCommandBuffer command{}; VkRenderPass pass{}; VkFramebuffer framebuffer{};
    VkPipelineLayout layout{}; VkPipeline pipeline{}; VkDescriptorPool descriptorPool{}; VkSampler sampler{};
    std::array<VkDescriptorSetLayout, 4> setLayouts{}; std::array<VkDescriptorSet, 4> sets{};
    std::vector<Buffer> buffers; std::vector<Image> images; std::vector<VkShaderModule> modules;
    GxVulkan::GpuAdmission bufferAdmission{16 * 1024 * 1024};
    GxVulkan::ResidentBuffer resident;
    std::vector<GxVulkan::Buffer> retired;
    uint64_t residentFrame = 0, residentDrawUploads = 0;
    Buffer vertices{}, vertexUniform{}, pixelUniform{}, alphaUniform{}, staging{}, readback{}; Image target{};
    Buffer previousUniform{}, jitterUniform{}, motionOptions{}, motionReadback{}; Image motionTarget{};
    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            resident.Destroy(device);
            for (auto& buffer : retired) GxVulkan::DestroyBuffer(device, buffer);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (pass) vkDestroyRenderPass(device, pass, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            for (auto set : setLayouts) if (set) vkDestroyDescriptorSetLayout(device, set, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
            for (auto image : images) { if (image.view) vkDestroyImageView(device, image.view, nullptr); if (image.handle) vkDestroyImage(device, image.handle, nullptr); if (image.memory) vkFreeMemory(device, image.memory, nullptr); }
            for (auto buffer : buffers) { if (buffer.mapped) vkUnmapMemory(device, buffer.memory); if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr); if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr); }
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No fixture memory type");
    }
    Buffer MakeBuffer(size_t size, VkBufferUsageFlags usage) {
        buffers.emplace_back(); auto& result = buffers.back();
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
        Vk(vkCreateBuffer(device, &info, nullptr, &result.handle));
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Vk(vkAllocateMemory(device, &allocate, nullptr, &result.memory)); Vk(vkBindBufferMemory(device, result.handle, result.memory, 0));
        Vk(vkMapMemory(device, result.memory, 0, size, 0, &result.mapped)); return result;
    }
    Image MakeImage(uint32_t size, VkImageUsageFlags usage,
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        images.emplace_back(); auto& result = images.back();
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format; info.extent = {size, size, 1}; info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
        Vk(vkCreateImage(device, &info, nullptr, &result.handle));
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Vk(vkAllocateMemory(device, &allocate, nullptr, &result.memory)); Vk(vkBindImageMemory(device, result.handle, result.memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format; view.subresourceRange = {aspect, 0, 1, 0, 1};
        Vk(vkCreateImageView(device, &view, nullptr, &result.view)); return result;
    }
    void Begin() {
        Vk(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; Vk(vkBeginCommandBuffer(command, &begin));
    }
    void Submit() {
        Vk(vkEndCommandBuffer(command)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        Vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); Vk(vkQueueWaitIdle(queue));
    }
    void Barrier(VkImage image, VkImageLayout oldLayout, VkImageLayout next, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before, VkPipelineStageFlags after,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image;
        barrier.oldLayout = oldLayout; barrier.newLayout = next; barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkShaderModule Module(const char* name) {
        std::ifstream stream(std::string(WOWDECOMP_SLANG_TEST_DIR) + '/' + name, std::ios::binary | std::ios::ate);
        const std::streamoff length = stream.tellg(); Check(stream && length >= 20 && length % 4 == 0, "Missing authored Slang module");
        std::vector<uint32_t> words(static_cast<size_t>(length) / 4); stream.seekg(0); stream.read(reinterpret_cast<char*>(words.data()), length);
        Check(static_cast<bool>(stream), "Cannot read authored Slang module");
        return Module(words);
    }
    VkShaderModule Module(const std::vector<uint32_t>& words) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = words.size() * 4; info.pCode = words.data();
        VkShaderModule module; Vk(vkCreateShaderModule(device, &info, nullptr, &module)); modules.push_back(module); return module;
    }
    void Init(bool temporal = false) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        Vk(vkCreateInstance(&create, nullptr, &instance));
        uint32_t count = 0; Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr)); Check(count != 0, "No Vulkan device for GPU regression test");
        std::vector<VkPhysicalDevice> devices(count); Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = 0; bool found = false;
        for (auto candidate : devices) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr); std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
            for (uint32_t i = 0; i < count; ++i) if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { physical = candidate; family = i; found = true; break; }
            if (found) break;
        }
        Check(found, "No graphics queue for GPU regression test"); vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        const float priority = 1; VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        Vk(vkCreateDevice(physical, &deviceInfo, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = family;
        Vk(vkCreateCommandPool(device, &pool, nullptr, &commandPool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandPool = commandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1; Vk(vkAllocateCommandBuffers(device, &allocate, &command));
        vertices = MakeBuffer(sizeof(Vertex) * 6, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        vertexUniform = MakeBuffer(sizeof(VertexConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        pixelUniform = MakeBuffer(sizeof(PixelConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        alphaUniform = MakeBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        std::memset(alphaUniform.mapped, 0, 16);
        staging = MakeBuffer(16 * 16 * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        readback = MakeBuffer(64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto base = MakeImage(16, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        auto mask = MakeImage(16, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        target = MakeImage(64, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (temporal) {
            previousUniform = MakeBuffer(sizeof(VertexConstants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            jitterUniform = MakeBuffer(16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            motionOptions = MakeBuffer(16,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            motionReadback = MakeBuffer(64*64*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            motionTarget = MakeImage(64,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_FORMAT_R32G32B32A32_SFLOAT);
            std::memset(previousUniform.mapped,0,sizeof(VertexConstants));
            std::memset(jitterUniform.mapped,0,16); std::memset(motionOptions.mapped,0,16);
        }
        auto* texels = static_cast<uint8_t*>(staging.mapped);
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const int offset = (y * 16 + x) * 4; texels[offset] = 128; texels[offset + 1] = 64; texels[offset + 2] = 32; texels[offset + 3] = 128;
            const int masked = offset + 1024; texels[masked] = texels[masked + 1] = texels[masked + 2] = 255;
            const float dx = (x + .5f) / 16 - .5f, dy = (y + .5f) / 16 - .5f;
            texels[masked + 3] = dx * dx + dy * dy < .16f ? 255 : 0;
        }
        Begin();
        for (unsigned i = 0; i < 2; ++i) {
            const auto image = i ? mask : base;
            Barrier(image.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.bufferOffset = i * 1024; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {16,16,1};
            vkCmdCopyBufferToImage(command, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        Submit();
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sample.magFilter = sample.minFilter = VK_FILTER_NEAREST;
        sample.addressModeU = sample.addressModeV = sample.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; Vk(vkCreateSampler(device, &sample, nullptr, &sampler));
        for (unsigned i = 0; i < 4; ++i) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            if (i == 1 || i == 3) bindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, i == 1 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            if (i == 3) bindings.push_back({1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            if (temporal && i == 1) {
                bindings.push_back({1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr});
                bindings.push_back({2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr});
            }
            if (temporal && i == 3) bindings.push_back({2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
            if (i == 2) for (unsigned slot = 0; slot < 8; ++slot) bindings.push_back({slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = static_cast<uint32_t>(bindings.size()); info.pBindings = bindings.data();
            Vk(vkCreateDescriptorSetLayout(device, &info, nullptr, &setLayouts[i]));
        }
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, temporal ? 6u : 3u}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}};
        VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; descriptors.maxSets = 4; descriptors.poolSizeCount = 2; descriptors.pPoolSizes = sizes;
        Vk(vkCreateDescriptorPool(device, &descriptors, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; set.descriptorPool = descriptorPool; set.descriptorSetCount = 4; set.pSetLayouts = setLayouts.data();
        Vk(vkAllocateDescriptorSets(device, &set, sets.data()));
        VkDescriptorBufferInfo uniform[] = {{vertexUniform.handle, 0, sizeof(VertexConstants)}, {pixelUniform.handle, 0, sizeof(PixelConstants)}, {alphaUniform.handle, 0, 16}};
        std::array<VkDescriptorImageInfo, 8> textures{}; std::vector<VkWriteDescriptorSet> writes;
        for (unsigned i = 0; i < 3; ++i) { VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = sets[i == 0 ? 1 : 3]; write.dstBinding = i == 2 ? 1 : 0; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &uniform[i]; writes.push_back(write); }
        for (unsigned i = 0; i < 8; ++i) { textures[i] = {sampler, i == 1 ? mask.view : base.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = sets[2]; write.dstBinding = i; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &textures[i]; writes.push_back(write); }
        if (temporal) {
            const VkDescriptorBufferInfo extra[] = {{previousUniform.handle,0,sizeof(VertexConstants)},
                {jitterUniform.handle,0,16},{motionOptions.handle,0,16}};
            for (unsigned i = 0; i < 3; ++i) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = sets[i == 2 ? 3 : 1];
                write.dstBinding = i == 0 ? 1 : 2; write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &extra[i];
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            }
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        std::array<VkAttachmentDescription,2> attachments{attachment,attachment};
        attachments[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        const VkAttachmentReference references[] = {reference,{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        const uint32_t targetCount = temporal ? 2 : 1;
        subpass.colorAttachmentCount = targetCount; subpass.pColorAttachments = references;
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; render.attachmentCount = targetCount; render.pAttachments = attachments.data(); render.subpassCount = 1; render.pSubpasses = &subpass;
        Vk(vkCreateRenderPass(device, &render, nullptr, &pass));
        const VkImageView views[] = {target.view,motionTarget.view};
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; frame.renderPass = pass; frame.attachmentCount = targetCount; frame.pAttachments = views; frame.width = frame.height = 64; frame.layers = 1;
        Vk(vkCreateFramebuffer(device, &frame, nullptr, &framebuffer));
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipelineLayout.setLayoutCount = 4; pipelineLayout.pSetLayouts = setLayouts.data(); Vk(vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &layout));
        if (!temporal) SetShaders(Module("fixed.vert.spv"), Module("fixed.frag.spv"));
    }
    void SetShaders(VkShaderModule vertex, VkShaderModule pixel, bool legacyVertex = false, uint32_t attributeMask = ~0u, bool subviewport = false) {
        if (pipeline) { vkDestroyPipeline(device, pipeline, nullptr); pipeline = VK_NULL_HANDLE; }
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = i ? pixel : vertex; stages[i].pName = "main"; }
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},{3,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},{4,0,VK_FORMAT_R8G8B8A8_UNORM,offsetof(Vertex,colour)}};
        for (unsigned i = 0; i < 8; ++i) attributes.push_back({6+i,0,VK_FORMAT_R32G32_SFLOAT,static_cast<uint32_t>(offsetof(Vertex,uv)+i*8)});
        if (legacyVertex) {
            std::vector<VkVertexInputAttributeDescription> used;
            for (auto attribute : attributes) if (attribute.location == 0 || attribute.location == 4 || attribute.location == 7) used.push_back(attribute);
            attributes = used;
        }
        attributes.erase(std::remove_if(attributes.begin(),attributes.end(),[&](const auto& attribute) {
            return !(attributeMask & (1u << attribute.location));
        }),attributes.end());
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding; input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()); input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport = subviewport ? VkViewport{16,48,32,-32,0,1} : VkViewport{0,motionTarget.handle ? 64.f : 0.f,64,motionTarget.handle ? -64.f : 64.f,0,1}; VkRect2D scissor{{0,0},{64,64}};
        VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; view.viewportCount = view.scissorCount = 1; view.pViewports = &viewport; view.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState colour{}; colour.colorWriteMask = 15;
        const std::array<VkPipelineColorBlendAttachmentState,2> colours{colour,colour};
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = motionTarget.handle ? 2 : 1; blend.pAttachments = colours.data();
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount = 2; info.pStages = stages; info.pVertexInputState = &input; info.pInputAssemblyState = &assembly;
        info.pViewportState = &view; info.pRasterizationState = &raster; info.pMultisampleState = &samples; info.pColorBlendState = &blend; info.layout = layout; info.renderPass = pass;
        Vk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    }
    void Draw(VertexConstants v, PixelConstants p, uint32_t colour = 0xFFFFFFFF, bool residentVertices = false) {
        std::memcpy(vertexUniform.mapped, &v, sizeof(v)); std::memcpy(pixelUniform.mapped, &p, sizeof(p));
        const float uv[6][2] = {{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}}; Vertex data[6]{};
        for (unsigned i = 0; i < 6; ++i) { data[i].position[0] = uv[i][0] * 2 - 1; data[i].position[1] = uv[i][1] * 2 - 1; data[i].normal[2] = 1; data[i].colour = colour; for (auto& coordinate : data[i].uv) { coordinate[0] = uv[i][0]; coordinate[1] = uv[i][1]; } }
        std::memcpy(vertices.mapped, data, sizeof(data)); Begin(); std::array<VkClearValue,2> clear{};
        clear[1].color.float32[2] = -1; // No geometry is not a valid previous-depth sample.
        const auto vertexBuffer = residentVertices ? resident.Resolve(device, memoryProperties, command,
            ++residentFrame, data, sizeof(data), retired, residentDrawUploads) : vertices.handle;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea.extent = {64,64}; begin.clearValueCount = motionTarget.handle ? 2 : 1; begin.pClearValues = clear.data();
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkDeviceSize offset = 0; vkCmdBindVertexBuffers(command, 0, 1, &vertexBuffer, &offset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, sets.data(), 0, nullptr); vkCmdDraw(command, 6, 1, 0, 0); vkCmdEndRenderPass(command);
        Barrier(target.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {64,64,1};
        vkCmdCopyImageToBuffer(command, target.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
        if (motionTarget.handle) {
            Barrier(motionTarget.handle,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyImageToBuffer(command,motionTarget.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,motionReadback.handle,1,&copy);
        }
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.buffer = readback.handle; host.size = VK_WHOLE_SIZE;
        std::array<VkBufferMemoryBarrier,2> hosts{host,host}; hosts[1].buffer = motionReadback.handle;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,motionTarget.handle ? 2 : 1,hosts.data(),0,nullptr); Submit();
    }
    void TestTemporalShaders() {
        constexpr uint32_t rigid[] = {0xFFFE0200,0x0200001F,0x80000000,0x900F0000,
            0x03000002,0xC00F0000,0x90E40000,0xA0E40000,0x0000FFFF};
        // Weighted bone translation plus a separate world/camera translation.
        constexpr uint32_t skinned[] = {0xFFFE0200,0x0200001F,0x80000000,0x900F0000,
            0x0200001F,0x80010005,0x900F0002,
            0x03000005,0x800F0000,0xA0E40001,0x90000002,
            0x03000002,0x800F0000,0x80E40000,0x90E40000,
            0x03000002,0xC00F0000,0x80E40000,0xA0E40000,0x0000FFFF};
        constexpr uint32_t pixel[] = {0xFFFF0200,0x02000001,0x800F0800,0xA0E40000,0x0000FFFF};
        GxVulkan::SlangCompiler compiler;
        const auto fragment = Module(compiler.Compile(GxVulkan::RecoverTemporalSlangShader(pixel,sizeof(pixel)).source,false));
        SetShaders(Module(compiler.Compile(GxVulkan::RecoverTemporalSlangShader(rigid,sizeof(rigid)).source,true)),fragment,true,1);
        VertexConstants v{}; PixelConstants p{}; p.options = {.2f,.4f,.6f,.8f};
        auto* options = static_cast<float*>(motionOptions.mapped); options[0] = 1; options[1] = .35f; options[2] = 0; options[3] = 1;
        const auto motion = [&](const std::array<float,4>& expected) {
            const auto* actual = static_cast<const float*>(motionReadback.mapped)+(32*64+32)*4;
            for (unsigned i = 0; i < 4; ++i)
                Check(std::isfinite(actual[i]) && std::fabs(actual[i]-expected[i])<.00001f,"Rendered temporal motion/validity mismatch");
        };
        v.projection[0] = .25f; v.projection[1] = .1f;
        Draw(v,p); Pixel(32,32,p.options); motion({.125f,-.05f,0,.35f});
        static_cast<float*>(previousUniform.mapped)[2] = .4f;
        Draw(v,p); motion({.125f,-.05f,.4f,.35f});
        static_cast<float*>(previousUniform.mapped)[2] = 0;
        options[0] = 0; Draw(v,p); motion({0,0,-1,.35f});
        options[0] = 1;
        static_cast<float*>(previousUniform.mapped)[3] = -2;
        Draw(v,p); motion({0,0,-1,.35f});
        static_cast<float*>(previousUniform.mapped)[3] = 0;
        static_cast<float*>(previousUniform.mapped)[0] = std::numeric_limits<float>::quiet_NaN();
        Draw(v,p); motion({0,0,-1,.35f});
        static_cast<float*>(previousUniform.mapped)[0] = std::numeric_limits<float>::max();
        static_cast<float*>(previousUniform.mapped)[3] = -.9999f;
        Draw(v,p); motion({0,0,-1,.35f});
        std::memset(previousUniform.mapped,0,sizeof(VertexConstants));
        v = {}; auto* jitter = static_cast<float*>(jitterUniform.mapped); jitter[0] = .125f; jitter[1] = -.25f;
        Draw(v,p); motion({0,0,0,.35f}); Pixel(1,32,{0,0,0,0}); Pixel(8,32,p.options);
        std::memset(jitterUniform.mapped,0,16);
        static_cast<float*>(alphaUniform.mapped)[0] = .9f;
        Draw(v,p); Pixel(32,32,{0,0,0,0}); motion({0,0,-1,0});
        static_cast<float*>(alphaUniform.mapped)[0] = 0;
        SetShaders(Module(compiler.Compile(GxVulkan::RecoverTemporalSlangShader(skinned,sizeof(skinned)).source,true)),fragment,true,1u|(1u<<7));
        v.projection[0] = .1f; v.projection[4] = .4f;
        auto* previous = static_cast<float*>(previousUniform.mapped); previous[0] = -.05f; previous[4] = -.2f;
        Draw(v,p); Pixel(32,32,p.options);
        const float currentNdcX = 2*(32.5f/64)-1;
        const float weight = (currentNdcX+1-.1f)/2.4f;
        motion({.075f+.3f*weight,0,0,.35f});
        std::cout << "Temporal GPU shaders: current/previous pose, weighted bone motion, jitter exclusion, alpha discard and invalid-history flags passed\n";
    }
    void TestTemporalBackend() {
        constexpr uint32_t rigid[] = {0xFFFE0200,0x0200001F,0x80000000,0x900F0000,
            0x03000002,0xC00F0000,0x90E40000,0xA0E40000,0x0000FFFF};
        constexpr uint32_t pixel[] = {0xFFFF0200,0x02000001,0x800F0800,0xA0E40000,0x0000FFFF};
        GxVulkan::SlangCompiler compiler;
        const auto recoveredVS = Module(compiler.Compile(GxVulkan::TemporalViewportSource(
            GxVulkan::RecoverTemporalSlangShader(rigid,sizeof(rigid)).source),true));
        const auto recoveredPS = Module(compiler.Compile(GxVulkan::RecoverTemporalSlangShader(pixel,sizeof(pixel)).source,false));
        const auto fixedVS = Module("fixed-temporal.vert.spv"), fixedPS = Module("fixed-temporal.frag.spv");
        auto* options = static_cast<float*>(motionOptions.mapped);
        options[0] = 1; options[1] = .25f; options[2] = .2f; options[3] = .8f;
        auto* jitter = static_cast<float*>(jitterUniform.mapped);
        const auto motion = [&](std::array<float,4> expected) {
            const auto* actual = static_cast<float*>(motionReadback.mapped)+(32*64+32)*4;
            for (unsigned i = 0; i < 4; ++i)
                Check(std::isfinite(actual[i]) && std::fabs(actual[i]-expected[i]) < .00001f,
                    "Backend fixed/recovered viewport motion mismatch");
        };
        for (bool subviewport : {false,true}) {
            const float scale = subviewport ? .5f : 1.f;
            jitter[0] = .03125f; jitter[1] = -.0625f; jitter[2] = jitter[3] = scale;
            VertexConstants v{}; PixelConstants p{}; p.options = {.2f,.4f,.6f,.8f};
            v.projection[0] = .25f; v.projection[1] = .125f; v.projection[2] = .3f;
            std::memset(previousUniform.mapped,0,sizeof(VertexConstants));
            static_cast<float*>(previousUniform.mapped)[2] = .4f;
            SetShaders(recoveredVS,recoveredPS,true,1,subviewport);
            Draw(v,p); Pixel(32,32,p.options); motion({.125f*scale,-.0625f*scale,.44f,.25f});
            VertexConstants previous{};
            previous.projection = Identity(); previous.projection[14] = .4f;
            v = {}; v.projection = Identity(); v.view = v.normal = Identity();
            v.projection[12] = .25f; v.projection[13] = .125f; v.projection[14] = .3f;
            std::memcpy(previousUniform.mapped,&previous,sizeof(previous));
            // Mixed authored VS/recovered PS keeps location 11/12 and material colour.
            SetShaders(fixedVS,recoveredPS,false,~0u,subviewport);
            Draw(v,p); Pixel(32,32,p.options); motion({.125f*scale,-.0625f*scale,.44f,.25f});
            SetShaders(fixedVS,fixedPS,false,~0u,subviewport);
            p = {}; Draw(v,p); Pixel(32,32,{1,1,1,1}); motion({.125f*scale,-.0625f*scale,.44f,.25f});
            options[0] = 0; Draw(v,p); motion({0,0,-1,.25f}); options[0] = 1;
            if (subviewport) Pixel(4,32,{0,0,0,0});
        }
        std::cout << "Backend temporal GPU: full/subviewport (nonzero origin), fixed/recovered/mixed stages, jitter exclusion, previous mapped depth and invalid ownership passed\n";
    }
    void TestWorldTargets() {
        const auto portrait = GxVulkan::WorldViewportRect({712,1236}, 0,1,0,1);
        Check(portrait.extent.width == 712 && portrait.extent.height == 1236, "World viewport used fixed aspect");
        bool rejected = false;
        try { GxVulkan::WorldViewportRect({64,64}, 0,1,0,std::numeric_limits<float>::quiet_NaN()); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "Non-finite world viewport accepted");
        for (unsigned size : {16u,64u,32u,64u}) {
            const size_t bytes = size * size * 4;
            const auto usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            auto output = MakeImage(size,usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            auto scene = MakeImage(size,usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            auto outputDepth = MakeImage(size,usage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT);
            auto sceneDepth = MakeImage(size,usage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT);
            auto original = MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            auto painted = MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            auto oldDepth = MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            auto newDepth = MakeBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            for (unsigned i = 0; i < size*size; ++i) {
                static_cast<uint32_t*>(original.mapped)[i] = 0xFF000000u | (i*7919u & 0xFFFFFFu);
                static_cast<uint32_t*>(painted.mapped)[i] = 0xFF2080F0u;
                static_cast<float*>(oldDepth.mapped)[i] = .25f;
                static_cast<float*>(newDepth.mapped)[i] = .5f + .25f * (i % size) / size;
            }
            const auto uploadImage = [&](Image image, VkImageAspectFlags aspect, Buffer data, bool first) {
                Barrier(image.handle, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,aspect);
                VkBufferImageCopy copy{}; copy.imageSubresource = {aspect,0,0,1}; copy.imageExtent = {size,size,1};
                vkCmdCopyBufferToImage(command,data.handle,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                Barrier(image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,aspect);
            };
            const auto readImage = [&](Image image, VkImageAspectFlags aspect) {
                Begin();
                Barrier(image.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,aspect);
                VkBufferImageCopy copy{}; copy.imageSubresource = {aspect,0,0,1}; copy.imageExtent = {size,size,1};
                vkCmdCopyImageToBuffer(command,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
                Barrier(image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,aspect);
                VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                host.buffer = readback.handle; host.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
                Submit();
            };
            Begin(); uploadImage(output,VK_IMAGE_ASPECT_COLOR_BIT,original,true); uploadImage(outputDepth,VK_IMAGE_ASPECT_DEPTH_BIT,oldDepth,true);
            Barrier(scene.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            Barrier(sceneDepth.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
            const VkRect2D full{{0,0},{size,size}};
            GxVulkan::CopyWorldImage(command,output.handle,scene.handle,VK_IMAGE_ASPECT_COLOR_BIT,full);
            GxVulkan::CopyWorldImage(command,outputDepth.handle,sceneDepth.handle,VK_IMAGE_ASPECT_DEPTH_BIT,full); Submit();
            readImage(scene,VK_IMAGE_ASPECT_COLOR_BIT); Check(std::memcmp(readback.mapped,original.mapped,bytes)==0,"World scope lost pre-existing colour");
            readImage(sceneDepth,VK_IMAGE_ASPECT_DEPTH_BIT); Check(std::memcmp(readback.mapped,oldDepth.mapped,bytes)==0,"World scope lost pre-existing depth");
            Begin(); uploadImage(scene,VK_IMAGE_ASPECT_COLOR_BIT,painted,false); uploadImage(sceneDepth,VK_IMAGE_ASPECT_DEPTH_BIT,newDepth,false);
            const auto rect = GxVulkan::WorldViewportRect({size,size},.25f,.75f,.25f,.75f);
            GxVulkan::CopyWorldImage(command,scene.handle,output.handle,VK_IMAGE_ASPECT_COLOR_BIT,rect);
            GxVulkan::CopyWorldImage(command,sceneDepth.handle,outputDepth.handle,VK_IMAGE_ASPECT_DEPTH_BIT,rect); Submit();
            for (unsigned depthCase = 0; depthCase < 2; ++depthCase) {
                readImage(depthCase ? outputDepth : output,depthCase ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
                for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
                    const auto i = y*size+x; const bool inside = x >= size/4 && x < size*3/4 && y >= size/4 && y < size*3/4;
                    const auto data = depthCase ? (inside ? newDepth : oldDepth) : (inside ? painted : original);
                    Check(std::memcmp(static_cast<uint8_t*>(readback.mapped)+i*4,static_cast<uint8_t*>(data.mapped)+i*4,4)==0,
                        "World composite changed outside-viewport UI or lost colour/depth bits");
                }
            }
        }
        std::cout << "World targets: viewport conversion, invalid input, exact colour/depth copies, contained composite and recreated sizes passed\n";
    }
    void TestResidency() {
        uint64_t uploaded = 0;
        const auto resolve = [&](uint64_t frame, const void* data, size_t bytes) {
            return resident.Resolve(device, memoryProperties, command, frame, data, bytes, retired, uploaded);
        };
        const auto copy = [&](VkBuffer source, VkDeviceSize offset, VkDeviceSize bytes, VkDeviceSize destination) {
            VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ready.buffer = source; ready.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &ready, 0, nullptr);
            const VkBufferCopy region{offset,destination,bytes};
            vkCmdCopyBuffer(command, source, readback.handle, 1, &region);
        };
        const auto submit = [&] {
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback.handle; host.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &host, 0, nullptr);
            Submit();
            for (auto& buffer : retired) GxVulkan::DestroyBuffer(device, buffer);
            retired.clear();
        };
        const auto equal = [&](size_t offset, const void* expected, size_t bytes) {
            Check(std::memcmp(static_cast<uint8_t*>(readback.mapped)+offset,expected,bytes)==0,
                "Resident GPU snapshot changed unexpectedly");
        };
        // Reservations follow physical allocation and recorded/submitted
        // ownership, not the logical handle or requested (unaligned) byte count.
        const uint32_t admittedData[] = {101,202,303};
        auto admitted = GxVulkan::CreateBuffer(device, memoryProperties, sizeof(admittedData),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &bufferAdmission);
        retired.push_back(admitted); // Fixture owns cleanup even if an assertion fails.
        Check(bufferAdmission.Used() == admitted.allocationBytes && admitted.allocationBytes >= admitted.size,
            "Buffer admission did not charge physical requirements");
        std::memcpy(admitted.mapped, admittedData, sizeof(admittedData));
        Begin();
        VkBufferMemoryBarrier hostSource{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostSource.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; hostSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hostSource.srcQueueFamilyIndex = hostSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostSource.buffer = admitted.buffer; hostSource.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&hostSource,0,nullptr);
        const VkBufferCopy admittedCopy{0,0,sizeof(admittedData)};
        vkCmdCopyBuffer(command,admitted.buffer,readback.handle,1,&admittedCopy);
        admitted = {};
        Check(bufferAdmission.Used() != 0,"Logical buffer retirement released physical reservation early");
        submit(); equal(0,admittedData,sizeof(admittedData));
        Check(bufferAdmission.Used() == 0,"Completed buffer destruction leaked reservation");
        GxVulkan::GpuAdmission pressure(1);
        bool rejected = false;
        try { auto unexpected = GxVulkan::CreateBuffer(device,memoryProperties,1024,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&pressure);
            GxVulkan::DestroyBuffer(device,unexpected);
        } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && pressure.Used() == 0,"Rejected physical admission leaked or allocated beyond pressure");
        const uint32_t a[] = {11,22,33}, b[] = {44,55,66}, c[] = {77,88,99};
        Begin();
        const auto first = resolve(1,a,sizeof(a));
        Check(uploaded == sizeof(a), "Initial resident upload count is incorrect");
        Check(resolve(1,a,sizeof(a)) == first && uploaded == sizeof(a), "Unchanged mesh was reuploaded");
        resident.MarkDirty();
        const auto second = resolve(1,b,sizeof(b));
        Check(first != second, "Mid-frame mutation overwrote an in-flight snapshot");
        copy(first,0,sizeof(a),0); copy(second,0,sizeof(b),16); submit();
        equal(0,a,sizeof(a)); equal(16,b,sizeof(b));
        Begin();
        const auto warmBytes = uploaded;
        for (unsigned draw = 0; draw < 20; ++draw)
            Check(resolve(2,b,sizeof(b)) == second, "Resident handle was not stable across frames");
        Check(uploaded == warmBytes, "Steady-state mesh draws performed another upload");
        copy(second,0,sizeof(b),0); submit(); equal(0,b,sizeof(b));
        Begin(); resident.MarkDirty();
        const auto updated = resolve(3,c,sizeof(c));
        Check(updated == second, "Completed-frame buffer was unnecessarily replaced");
        std::array<uint8_t,16> grown{}; for (unsigned i = 0; i < 13; ++i) grown[i] = static_cast<uint8_t>(100+i);
        const auto resized = resolve(3,grown.data(),13);
        Check(resized != updated, "Resize replaced data still referenced in this frame");
        copy(updated,0,sizeof(c),0); copy(resized,0,16,16); submit();
        equal(0,c,sizeof(c)); equal(16,grown.data(),16);
        Begin(); grown[13] = 201;
        const auto resizedWithinPadding = resolve(4,grown.data(),14);
        Check(resizedWithinPadding == resized, "Same-capacity update should reuse a completed buffer");
        copy(resizedWithinPadding,0,16,0);
        resident.Retire(retired); // Logical destruction before submission must preserve the recorded read.
        submit(); equal(0,grown.data(),16);
        Begin();
        std::vector<uint8_t> large(68*1024*1024,0x5A); large.back() = 0xC3;
        const auto largeBuffer = resolve(5,large.data(),large.size());
        const auto afterLargeUpload = uploaded;
        for (unsigned draw = 0; draw < 20; ++draw) resolve(5,large.data(),large.size());
        Check(uploaded == afterLargeUpload, "Large resident mesh was repeatedly copied");
        copy(largeBuffer,large.size()-16,16,0); submit(); equal(0,large.data()+large.size()-16,16);
        VertexConstants vertex{}; PixelConstants pixel{};
        vertex.projection = vertex.view = vertex.normal = Identity();
        for (auto& matrix : vertex.texture) matrix = Identity();
        residentFrame = 5;
        Draw(vertex,pixel,0xFF0000FF,true); Pixel(32,32,{0,0,1,1});
        const auto firstDrawUpload = residentDrawUploads;
        Draw(vertex,pixel,0xFF0000FF,true); Pixel(32,32,{0,0,1,1});
        Check(residentDrawUploads == firstDrawUpload, "Resident rendered geometry was reuploaded");
        resident.MarkDirty();
        Draw(vertex,pixel,0xFFFF0000,true); Pixel(32,32,{1,0,0,1});
        Check(residentDrawUploads > firstDrawUpload, "Mutated rendered geometry was not uploaded");
        std::cout << "GPU residency: retained data, mutation, resize, deferred destruction, 68 MiB mesh and rendered pixels passed\n";
    }
    void Pixel(unsigned x, unsigned y, const std::array<float,4>& expected) {
        const auto* bytes = static_cast<const uint8_t*>(readback.mapped) + (y * 64 + x) * 4;
        for (unsigned i = 0; i < 4; ++i) if (std::fabs(bytes[i] - expected[i] * 255) > 2)
            throw std::runtime_error("GPU colour mismatch at channel " + std::to_string(i) + ": " + std::to_string(bytes[i]) + " expected " + std::to_string(expected[i]*255));
    }
};
std::array<float,4> Combine(const std::array<float,4>& tex, const std::array<float,4>& current, const std::array<float,4>& diffuse, unsigned operation) {
    std::array<float,4> result{};
    for (unsigned i = 0; i < 4; ++i) {
        float value = 0;
        switch (operation) { case 0:value=tex[i]*current[i];break; case 1:value=2*tex[i]*current[i];break; case 2:value=tex[i]+current[i];break; case 3:value=current[i];break; case 4:value=current[i]*current[3]+tex[i]*(1-current[3]);break; case 5:value=tex[i]*diffuse[3]+current[i]*(1-diffuse[3]);break; }
        result[i] = std::fmax(0.f,std::fmin(1.f,value));
    }
    return result;
}
}
int main(int argc, char** argv) {
    try {
        const bool backend = argc == 2 && std::strcmp(argv[1],"--temporal-backend") == 0;
        const bool temporal = backend || (argc == 2 && std::strcmp(argv[1],"--temporal") == 0);
        Fixture fixture; fixture.Init(temporal);
        if (backend) { fixture.TestTemporalBackend(); return 0; }
        if (temporal) { fixture.TestTemporalShaders(); return 0; }
        if (argc == 2 && std::strcmp(argv[1],"--residency") == 0) { fixture.TestResidency(); return 0; }
        if (argc == 2 && std::strcmp(argv[1],"--world-target") == 0) { fixture.TestWorldTargets(); return 0; }
        VertexConstants v{}; PixelConstants p{};
        v.projection = v.view = v.normal = Identity(); for (auto& matrix : v.texture) matrix = Identity();
        // Low-detail terrain supplies white vertices and a full-fog interval.
        p.fog={.1f,.4f,.7f,1}; v.fog={1,0,1,0}; v.view[14]=10;
        fixture.Draw(v,p); fixture.Pixel(32,32,{.1f,.4f,.7f,1});
        v.fog={1,5,15,0};
        v.view[14]=2; fixture.Draw(v,p); fixture.Pixel(32,32,{1,1,1,1});
        v.view[14]=10; fixture.Draw(v,p); fixture.Pixel(32,32,{.55f,.7f,.85f,1});
        v.view[14]=-10; fixture.Draw(v,p); fixture.Pixel(32,32,{.55f,.7f,.85f,1});
        v.view[14]=20; fixture.Draw(v,p,0x80FFFFFF); fixture.Pixel(32,32,{.1f,.4f,.7f,128.f/255});
        p.fog[3]=0; fixture.Draw(v,p); fixture.Pixel(32,32,{1,1,1,1});
        p.fog[3]=1; v.fog[0]=0; fixture.Draw(v,p); fixture.Pixel(32,32,{1,1,1,1});
        v.fog={1,10,10,0}; v.view[14]=9; fixture.Draw(v,p); fixture.Pixel(32,32,{1,1,1,1});
        v.view[14]=10; fixture.Draw(v,p); fixture.Pixel(32,32,{.1f,.4f,.7f,1});
        v.fog={}; p.fog={}; v.view=Identity();
        fixture.Draw(v,p,0xFF0000FF); fixture.Pixel(32,32,{0,0,1,1});
        v.control[0] = {0,0,0,1}; p.stage[0] = {1,0,0,0}; p.options[0] = .1f;
        fixture.Draw(v,p); const std::array<float,4> base{128.f/255,64.f/255,32.f/255,128.f/255}; fixture.Pixel(32,32,base);
        v.control[1] = {1,0,0,1}; p.stage[1] = {1,0,0,0}; fixture.Draw(v,p);
        fixture.Pixel(32,32,base); fixture.Pixel(3,3,{0,0,0,0});
        v.texture[1][12] = .5f; v.control[1][2] = 1; fixture.Draw(v,p);
        fixture.Pixel(32,32,{0,0,0,0}); fixture.Pixel(8,32,base);
        v.texture[1] = Identity(); v.control[1][2] = 0; p.stage[1] = {}; p.options[0] = 0;
        for (unsigned stage = 1; stage < 8; ++stage) {
            v.control[stage] = {static_cast<float>(stage),0,0,1};
            p.stage[stage] = {1,3,3,0};
        }
        p.stage[7] = {1,2,3,0}; fixture.Draw(v,p);
        fixture.Pixel(32,32,{1,128.f/255,64.f/255,128.f/255});
        for (unsigned stage = 1; stage < 8; ++stage) p.stage[stage] = {};
        const std::array<float,4> diffuse{64.f/255,128.f/255,192.f/255,64.f/255};
        for (unsigned operation = 0; operation < 6; ++operation) {
            p.stage[0] = {1,static_cast<float>(operation),static_cast<float>(operation),0};
            fixture.Draw(v,p,0x404080C0); fixture.Pixel(32,32,Combine(base,diffuse,diffuse,operation));
            p.stage[0] = {1,0,0,0}; p.stage[1] = {1,static_cast<float>(operation),static_cast<float>(operation),0};
            fixture.Draw(v,p,0x404080C0); fixture.Pixel(32,32,Combine({1,1,1,1},Combine(base,diffuse,diffuse,0),diffuse,operation)); p.stage[1] = {};
        }
        // Synthetic SM2: position/colour passthrough and TEXCOORD1 for the mask.
        // Test both mixed-stage directions using genuinely recovered Slang.
        constexpr uint32_t legacyVertex[] = {
            0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000,
            0x0200001F, 0x8000000A, 0x900F0001,
            0x0200001F, 0x80010005, 0x900F0002,
            0x02000001, 0xC00F0000, 0x90E40000,
            0x02000001, 0xD00F0000, 0x90E40001,
            0x02000001, 0xE00F0001, 0x90E40002, 0x0000FFFF
        };
        constexpr uint32_t legacyPixel[] = {0xFFFF0200, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF};
        GxVulkan::SlangCompiler compiler;
        fixture.SetShaders(fixture.Module(compiler.Compile(GxVulkan::RecoverSlangSource(legacyVertex,sizeof(legacyVertex)),true)),fixture.Module("fixed.frag.spv"),true);
        p = {}; p.options[0] = .1f; p.stage[0] = p.stage[1] = {1,0,0,0};
        fixture.Draw(v,p); fixture.Pixel(32,32,base); fixture.Pixel(3,3,{0,0,0,0});
        fixture.SetShaders(fixture.Module("fixed.vert.spv"),fixture.Module(compiler.Compile(GxVulkan::RecoverSlangSource(legacyPixel,sizeof(legacyPixel)),false)));
        p.options = {.2f,.4f,.6f,.8f}; fixture.Draw(v,p); fixture.Pixel(32,32,p.options);
        std::cout << "GPU fixed-function colour, mask, UV/transform, six texture operations and mixed shader stages passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
