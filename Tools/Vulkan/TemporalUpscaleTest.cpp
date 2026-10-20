#include <vulkan/vulkan.h>
#include "Engine/Source/Gx/CGxDeviceVulkan/TemporalBackend.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/WorldTarget.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t Size = 32;
using Pixel = std::array<float,4>;
using Pixels = std::vector<Pixel>;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Vk(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Vulkan fixture error " + std::to_string(result)); }
struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
// Matches the separate TemporalUpscale.slang std140 ABI.
struct alignas(16) Constants {
    std::array<uint32_t,4> extentValidity{Size,Size,1,0};
    std::array<int32_t,4> viewport{0,0,Size,Size}, previousViewport{0,0,Size,Size};
    Pixel accumulation{.9f,.001f,0,0}, reserved{0,0,1,0};
    std::array<uint32_t,4> renderExtent{16,16,0,0};
    std::array<int32_t,4> renderViewport{0,0,16,16};
    Pixel previousJitter{};
};
static_assert(alignof(Constants)==16 && sizeof(Constants)==128);
static_assert(offsetof(Constants,extentValidity)==0 && offsetof(Constants,viewport)==16 &&
    offsetof(Constants,previousViewport)==32 && offsetof(Constants,accumulation)==48 && offsetof(Constants,reserved)==64);
struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void* mapped{}; };
struct Image { VkImage handle{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct Fixture {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkCommandPool commandPool{}; VkCommandBuffer command{}; VkRenderPass pass{}; VkFramebuffer framebuffer{};
    VkPipelineLayout layout{}; VkPipeline pipeline{}; VkDescriptorPool descriptorPool{}; VkSampler sampler{};
    VkDescriptorSetLayout setLayout{}; VkDescriptorSet set{};
    std::vector<Buffer> buffers; std::vector<Image> images; std::vector<VkShaderModule> modules;
    Buffer uniform{}, staging{}, readback{};
    uint32_t internalSize=16;
    std::array<Image,5> input{}; Image target{};
    bool uploaded=false;
    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
            if (framebuffer) vkDestroyFramebuffer(device,framebuffer,nullptr);
            if (pass) vkDestroyRenderPass(device,pass,nullptr);
            if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            if (setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
            if (sampler) vkDestroySampler(device,sampler,nullptr);
            for (auto module:modules) vkDestroyShaderModule(device,module,nullptr);
            for (auto image:images) { if(image.view) vkDestroyImageView(device,image.view,nullptr); if(image.handle) vkDestroyImage(device,image.handle,nullptr); if(image.memory) vkFreeMemory(device,image.memory,nullptr); }
            for (auto buffer:buffers) { if(buffer.mapped) vkUnmapMemory(device,buffer.memory); if(buffer.handle) vkDestroyBuffer(device,buffer.handle,nullptr); if(buffer.memory) vkFreeMemory(device,buffer.memory,nullptr); }
            if (commandPool) vkDestroyCommandPool(device,commandPool,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if(instance) vkDestroyInstance(instance,nullptr);
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
    Image MakeImage(uint32_t size, VkImageUsageFlags usage) {
        images.emplace_back(); auto& result = images.back();
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32G32B32A32_SFLOAT; info.extent = {size, size, 1}; info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
        Vk(vkCreateImage(device, &info, nullptr, &result.handle));
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = Memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Vk(vkAllocateMemory(device, &allocate, nullptr, &result.memory)); Vk(vkBindImageMemory(device, result.handle, result.memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.handle; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
    void Barrier(VkImage image, VkImageLayout oldLayout, VkImageLayout next, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags before, VkPipelineStageFlags after) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image;
        barrier.oldLayout = oldLayout; barrier.newLayout = next; barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkShaderModule Module(const char* name) {
        std::ifstream stream(std::string(WOWDECOMP_TEMPORAL_SHADER_DIR) + '/' + name, std::ios::binary | std::ios::ate);
        const std::streamoff length = stream.tellg(); Check(stream && length >= 20 && length % 4 == 0, "Missing authored Slang module");
        std::vector<uint32_t> words(static_cast<size_t>(length) / 4); stream.seekg(0); stream.read(reinterpret_cast<char*>(words.data()), length);
        Check(static_cast<bool>(stream), "Cannot read authored Slang module");
        return Module(words);
    }
    VkShaderModule Module(const std::vector<uint32_t>& words) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = words.size() * 4; info.pCode = words.data();
        VkShaderModule module; Vk(vkCreateShaderModule(device, &info, nullptr, &module)); modules.push_back(module); return module;
    }
    void Init(uint32_t internal) {
        internalSize=internal;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        Vk(vkCreateInstance(&create, nullptr, &instance));
        uint32_t count = 0; Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr)); if (!count) throw Unsupported("No Vulkan device");
        std::vector<VkPhysicalDevice> devices(count); Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = 0; bool found = false;
        for (auto candidate : devices) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr); std::vector<VkQueueFamilyProperties> queues(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
            for (uint32_t i = 0; i < count; ++i) if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { physical = candidate; family = i; found = true; break; }
            if (found) break;
        }
        if (!found) throw Unsupported("No graphics queue"); vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        const float priority = 1; VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        Vk(vkCreateDevice(physical, &deviceInfo, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = family;
        Vk(vkCreateCommandPool(device, &pool, nullptr, &commandPool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandPool = commandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1; Vk(vkAllocateCommandBuffers(device, &allocate, &command));
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
        if (properties.apiVersion < VK_API_VERSION_1_1) throw Unsupported("Vulkan 1.1 required");
        VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
        const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((format.optimalTilingFeatures & needed)!=needed) throw Unsupported("RGBA32F sampled/attachment/transfer unavailable");
        std::cout << "Device: " << properties.deviceName << '\n';
        uniform=MakeBuffer(sizeof(Constants),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        staging=MakeBuffer(Size*Size*sizeof(Pixel)*5,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        readback=MakeBuffer(Size*Size*sizeof(Pixel),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for(unsigned i=0;i<5;++i) input[i]=MakeImage(i==0 || i==2 || i==4 ? internalSize : Size,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        target=MakeImage(Size,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sample.magFilter=sample.minFilter=VK_FILTER_NEAREST;
        sample.addressModeU=sample.addressModeV=sample.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Vk(vkCreateSampler(device,&sample,nullptr,&sampler));
        std::array<VkDescriptorSetLayoutBinding,7> bindings{};
        for(unsigned i=0;i<7;++i) bindings[i]={i,i<4 || i==6?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:i==4?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; setInfo.bindingCount=bindings.size(); setInfo.pBindings=bindings.data();
        Vk(vkCreateDescriptorSetLayout(device,&setInfo,nullptr,&setLayout));
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,5},{VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets=1; poolInfo.poolSizeCount=3; poolInfo.pPoolSizes=sizes;
        Vk(vkCreateDescriptorPool(device,&poolInfo,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; alloc.descriptorPool=descriptorPool; alloc.descriptorSetCount=1; alloc.pSetLayouts=&setLayout;
        Vk(vkAllocateDescriptorSets(device,&alloc,&set));
        std::array<VkDescriptorImageInfo,7> imageInfo{}; std::array<VkWriteDescriptorSet,7> writes{};
        VkDescriptorBufferInfo bufferInfo{uniform.handle,0,sizeof(Constants)};
        for(unsigned i=0;i<7;++i) {
            auto& w=writes[i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set; w.dstBinding=i; w.descriptorCount=1; w.descriptorType=bindings[i].descriptorType;
            if(i!=5) { imageInfo[i]={i==4?sampler:VK_NULL_HANDLE,i==6?input[4].view:i<4?input[i].view:VK_NULL_HANDLE,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; w.pImageInfo=&imageInfo[i]; }
            else w.pBufferInfo=&bufferInfo;
        }
        vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
        VkAttachmentDescription attachment{}; attachment.format=VK_FORMAT_R32G32B32A32_SFLOAT; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass{};
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount=1; subpass.pColorAttachments=&reference;
        // Repeated draws discard output but must still order prior transfer reads.
        VkSubpassDependency dependency{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0};
        VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; render.attachmentCount=1; render.pAttachments=&attachment; render.subpassCount=1; render.pSubpasses=&subpass; render.dependencyCount=1; render.pDependencies=&dependency;
        Vk(vkCreateRenderPass(device,&render,nullptr,&pass));
        VkFramebufferCreateInfo frame{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; frame.renderPass=pass; frame.attachmentCount=1; frame.pAttachments=&target.view; frame.width=frame.height=Size; frame.layers=1;
        Vk(vkCreateFramebuffer(device,&frame,nullptr,&framebuffer));
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipelineLayout.setLayoutCount=1; pipelineLayout.pSetLayouts=&setLayout;
        Vk(vkCreatePipelineLayout(device,&pipelineLayout,nullptr,&layout));
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(int i=0;i<2;++i) { stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=Module(i?"temporal-upscale.frag.spv":"temporal-upscale.vert.spv"); stages[i].pName="main"; }
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,Size,Size,0,1}; VkRect2D scissor{{0,0},{Size,Size}};
        VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; view.viewportCount=view.scissorCount=1; view.pViewports=&viewport; view.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState colour{}; colour.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&colour;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipelineInfo.stageCount=2; pipelineInfo.pStages=stages; pipelineInfo.pVertexInputState=&vertex; pipelineInfo.pInputAssemblyState=&assembly; pipelineInfo.pViewportState=&view; pipelineInfo.pRasterizationState=&raster; pipelineInfo.pMultisampleState=&samples; pipelineInfo.pColorBlendState=&blend; pipelineInfo.layout=layout; pipelineInfo.renderPass=pass;
        Vk(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline));
    }
    Pixels Draw(const Pixels& current,const Pixels& history,const Pixels& motion,const Pixels& depth,const Pixels& currentDepth,Constants constants) {
        const Pixels* data[]={&current,&history,&motion,&depth,&currentDepth};
        for(unsigned i=0;i<5;++i) {
            const auto size=i==0 || i==2 || i==4 ? internalSize : Size;
            Check(data[i]->size()==size*size,"Input extent mismatch");
            std::memcpy(static_cast<char*>(staging.mapped)+i*Size*Size*sizeof(Pixel),data[i]->data(),size*size*sizeof(Pixel));
        }
        std::memcpy(uniform.mapped,&constants,sizeof(constants)); Begin();
        for(unsigned i=0;i<5;++i) {
            const auto size=i==0 || i==2 || i==4 ? internalSize : Size;
            Barrier(input[i].handle,uploaded?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uploaded?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,uploaded?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.bufferOffset=i*Size*Size*sizeof(Pixel); copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={size,size,1};
            vkCmdCopyBufferToImage(command,staging.handle,input[i].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            Barrier(input[i].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass=pass; render.framebuffer=framebuffer; render.renderArea.extent={Size,Size};
        vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr); vkCmdDraw(command,3,1,0,0); vkCmdEndRenderPass(command);
        Barrier(target.handle,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={Size,Size,1};
        vkCmdCopyImageToBuffer(command,target.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.buffer=readback.handle; host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); Submit(); uploaded=true;
        Pixels result(Size*Size); std::memcpy(result.data(),readback.mapped,Size*Size*sizeof(Pixel)); return result;
    }
};
// Hard diagonal edges and subpixel-thin strips, integrated at 32x32 samples per
// output pixel. Both static and translated sequences use the same truth/function.
float Signal(float x,float y) {
    const float d=x+.37f*y;
    return (std::fmod(d+80.f,7.1f)<.8f || (x-.23f*y>12.3f && x-.23f*y<16.8f)) ? 1.f : 0.f;
}
Pixels Scene(uint32_t size,float jx,float jy,float translation) {
    Pixels result(size*size);
    for(unsigned y=0;y<size;++y) for(unsigned x=0;x<size;++x) {
        float c=Signal((x+.5f-jx)*Size/size-translation,(y+.5f-jy)*Size/size);
        result[y*size+x]={c,c,c,1};
    }
    return result;
}
Pixels Truth(float translation) {
    Pixels result(Size*Size);
    for(unsigned y=0;y<Size;++y) for(unsigned x=0;x<Size;++x) {
        float value=0;
        for(int sy=0;sy<32;++sy) for(int sx=0;sx<32;++sx)
            value+=Signal(x+(sx+.5f)/32-translation,y+(sy+.5f)/32);
        value/=1024;
        result[y*Size+x]={value,value,value,1};
    }
    return result;
}
double Error(const Pixels& a,const Pixels& b) {
    double result=0;
    for(unsigned y=3;y<Size-3;++y) for(unsigned x=3;x<Size-3;++x) {
        const double d=a[y*Size+x][0]-b[y*Size+x][0]; result+=d*d;
    }
    return result/((Size-6)*(Size-6));
}
void Quality(uint32_t internal,float velocity) {
    Fixture f; f.Init(internal);
    Constants c; c.renderExtent={internal,internal,0,0};
    c.renderViewport={0,0,int(internal),int(internal)};
    Pixels history(Size*Size,{0,0,0,1}), previousDepth(Size*Size,{.5f,0,0,0});
    Pixels depth(internal*internal,{.5f,0,0,0});
    Pixels motion(internal*internal,{velocity/Size,0,.5f,0});
    double temporalError=0,spatialError=0;
    for(unsigned frame=0;frame<96;++frame) {
        const auto jitter=GxVulkan::TemporalUpscaleJitter(frame);
        const float jx=frame ? jitter[0] : 0, jy=frame ? jitter[1] : 0;
        c.reserved={jx,jy,1,0}; c.extentValidity[2]=frame ? 1 : 0;
        const float translation=velocity*frame;
        history=f.Draw(Scene(internal,jx,jy,translation),history,motion,previousDepth,depth,c);
        c.previousJitter={jx,jy,0,0};
        if(frame>=64) {
            auto spatial=c; spatial.reserved={0,0,0,0}; spatial.extentValidity[2]=0;
            auto baseline=f.Draw(Scene(internal,0,0,translation),history,motion,previousDepth,depth,spatial);
            const auto truth=Truth(translation);
            temporalError+=Error(history,truth); spatialError+=Error(baseline,truth);
        }
    }
    std::cout << "scale=" << float(internal)/Size << " velocity=" << velocity
        << " temporal MSE=" << temporalError/32 << " spatial MSE=" << spatialError/32 << '\n';
    Check(temporalError<spatialError*.95,"Temporal reconstruction did not improve matched spatial MSE by 5%");
    // Reset, unknown, reactive, disoccluded, non-finite motion and current depth
    // must not import conspicuous unrelated history.
    Pixels fresh(internal*internal,{.2f,.3f,.4f,1});
    for(unsigned y=0;y<internal;++y) for(unsigned x=0;x<internal;++x)
        fresh[y*internal+x][0]=float(x%3)/2;
    const Pixels poisoned(Size*Size,{1,0,1,1});
    for(int rejection=0;rejection<8;++rejection) {
        c.reserved={.2f,-.3f,1,0}; c.extentValidity[2]=rejection==0 ? 0 : 1;
        auto m=motion; auto pd=previousDepth; auto d=depth;
        if(rejection==1) for(auto& v:m) v[2]=-1;
        if(rejection==2) for(auto& v:m) v[3]=1;
        if(rejection==3) for(auto& v:pd) v[0]=.8f;
        if(rejection==4) for(auto& v:m) v[0]=std::numeric_limits<float>::quiet_NaN();
        if(rejection==5) for(auto& v:d) v[0]=std::numeric_limits<float>::quiet_NaN();
        c.previousViewport=rejection==6 ? std::array<int32_t,4>{0,0,1,1} : std::array<int32_t,4>{0,0,Size,Size};
        if(rejection==7) for(unsigned y=0;y<internal;++y) for(unsigned x=0;x<internal;++x) d[y*internal+x][0]=x%2 ? .8f : .5f;
        auto result=f.Draw(fresh,poisoned,m,pd,d,c);
        auto reset=c; reset.extentValidity[2]=0;
        const auto expected=f.Draw(fresh,poisoned,m,pd,d,reset);
        for(unsigned i=0;i<result.size();++i) for(int channel=0;channel<4;++channel)
            Check(std::isfinite(result[i][channel]) && std::abs(result[i][channel]-expected[i][channel])<1e-5f,
                "Rejected history contaminated reconstruction");
    }
    // History colour is unjittered while previous depth was blitted from the
    // jittered internal grid. A half-internal-pixel prior jitter moves the depth
    // lookup one output pixel at 50%, and 2/3 pixel at 75%.
    c.previousViewport={0,0,Size,Size}; c.extentValidity[2]=1;
    c.reserved={0,0,1,0}; c.previousJitter={float(internal)/Size,0,0,0};
    Pixels matchingMotion(internal*internal,{0,0,.5f,0});
    Pixels selectiveDepth(Size*Size,{.8f,0,0,0});
    selectiveDepth[16*Size+17][0]=.5f;
    const Pixels neutralHistory(Size*Size,{.65f,.3f,.4f,1});
    const auto shifted=f.Draw(fresh,neutralHistory,matchingMotion,selectiveDepth,depth,c);
    c.previousJitter={};
    const auto unshifted=f.Draw(fresh,neutralHistory,matchingMotion,selectiveDepth,depth,c);
    auto reset=c; reset.extentValidity[2]=0;
    const auto expected=f.Draw(fresh,neutralHistory,matchingMotion,selectiveDepth,depth,reset);
    Check(std::abs(unshifted[16*Size+16][0]-expected[16*Size+16][0])<1e-5f,
        "Mismatching expected previous device depth was accepted");
    Check(std::abs(shifted[16*Size+16][0]-expected[16*Size+16][0])>.01f,
        "Previous jitter/render-to-output depth mapping was ignored");
}
}
int main() {
    try {
        const auto half=GxVulkan::WorldRenderExtent({321,241},.5f,1024);
        const auto threeQuarters=GxVulkan::WorldRenderExtent({321,241},.75f,1024);
        Check(half.width==161 && half.height==121 && threeQuarters.width==241 && threeQuarters.height==181,
            "Odd-sized scene extent rounding changed");
        const auto tiny=GxVulkan::WorldRenderExtent({1,1},.5f,1024);
        Check(tiny.width==1 && tiny.height==1,"Tiny scene extent became empty");
        for(float invalid:{.49f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            bool rejected=false; try { GxVulkan::WorldRenderExtent({320,240},invalid,1024); }
            catch(const std::invalid_argument&) { rejected=true; }
            Check(rejected,"Invalid scene extent accepted");
        }
        bool rejected=false; try { GxVulkan::WorldRenderExtent({2048,240},.5f,1024); }
        catch(const std::invalid_argument&) { rejected=true; }
        Check(rejected,"Output extent beyond device limit accepted");
        std::array<float,2> mean{};
        for(unsigned i=0;i<32;++i) { auto j=GxVulkan::TemporalUpscaleJitter(i);
            for(unsigned axis=0;axis<2;++axis) { Check(std::isfinite(j[axis]) && std::abs(j[axis])<=.5f,"Unbounded upscale jitter"); mean[axis]+=j[axis]; }
        }
        Check(std::abs(mean[0])<1e-5f && std::abs(mean[1])<1e-5f,"Uncentred upscale jitter");
        for(auto size:{16u,24u}) for(float velocity:{0.f,.0625f}) Quality(size,velocity);
    }
    catch(const Unsupported& e) { std::cout << e.what() << '\n'; return 77; }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
