#include <vulkan/vulkan.h>
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
// Matches TemporalResolve.slang's std140 ABI, bounds are min-inclusive/max-exclusive.
struct alignas(16) Constants {
    std::array<uint32_t,4> extentValidity{Size,Size,1,0};
    std::array<int32_t,4> viewport{0,0,Size,Size}, previousViewport{0,0,Size,Size};
    Pixel accumulation{.8f,.001f,0,0}, reserved{};
};
static_assert(alignof(Constants)==16 && sizeof(Constants)==80);
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
    std::array<Image,4> input{}; Image target{};
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
    void Init() {
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
        staging=MakeBuffer(Size*Size*sizeof(Pixel)*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        readback=MakeBuffer(Size*Size*sizeof(Pixel),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for(auto& image:input) image=MakeImage(Size,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        target=MakeImage(Size,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sample.magFilter=sample.minFilter=VK_FILTER_NEAREST;
        sample.addressModeU=sample.addressModeV=sample.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Vk(vkCreateSampler(device,&sample,nullptr,&sampler));
        std::array<VkDescriptorSetLayoutBinding,6> bindings{};
        for(unsigned i=0;i<6;++i) bindings[i]={i,i<4?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:i==4?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; setInfo.bindingCount=bindings.size(); setInfo.pBindings=bindings.data();
        Vk(vkCreateDescriptorSetLayout(device,&setInfo,nullptr,&setLayout));
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,4},{VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets=1; poolInfo.poolSizeCount=3; poolInfo.pPoolSizes=sizes;
        Vk(vkCreateDescriptorPool(device,&poolInfo,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; alloc.descriptorPool=descriptorPool; alloc.descriptorSetCount=1; alloc.pSetLayouts=&setLayout;
        Vk(vkAllocateDescriptorSets(device,&alloc,&set));
        std::array<VkDescriptorImageInfo,5> imageInfo{}; std::array<VkWriteDescriptorSet,6> writes{};
        VkDescriptorBufferInfo bufferInfo{uniform.handle,0,sizeof(Constants)};
        for(unsigned i=0;i<6;++i) {
            auto& w=writes[i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set; w.dstBinding=i; w.descriptorCount=1; w.descriptorType=bindings[i].descriptorType;
            if(i<5) { imageInfo[i]={i==4?sampler:VK_NULL_HANDLE,i<4?input[i].view:VK_NULL_HANDLE,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; w.pImageInfo=&imageInfo[i]; }
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
        for(int i=0;i<2;++i) { stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=Module(i?"temporal-resolve.frag.spv":"temporal-resolve.vert.spv"); stages[i].pName="main"; }
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
    Pixels Draw(const Pixels& current,const Pixels& history,const Pixels& motion,const Pixels& depth,Constants constants={}) {
        const Pixels* data[]={&current,&history,&motion,&depth};
        for(unsigned i=0;i<4;++i) { Check(data[i]->size()==Size*Size,"Input extent mismatch"); std::memcpy(static_cast<char*>(staging.mapped)+i*Size*Size*sizeof(Pixel),data[i]->data(),Size*Size*sizeof(Pixel)); }
        std::memcpy(uniform.mapped,&constants,sizeof(constants)); Begin();
        for(unsigned i=0;i<4;++i) {
            Barrier(input[i].handle,uploaded?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uploaded?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,uploaded?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.bufferOffset=i*Size*Size*sizeof(Pixel); copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={Size,Size,1};
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
Pixels Solid(Pixel value) { return Pixels(Size*Size,value); }
void Exact(const Pixels& actual,const Pixels& expected,const char* label) {
    if(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(Pixel))) throw std::runtime_error(std::string("Not exact fresh current: ")+label);
}
void Near(float actual,float expected,const char* label) { if(!std::isfinite(actual) || std::abs(actual-expected)>2e-6f) throw std::runtime_error(std::string(label)+": "+std::to_string(actual)+" expected "+std::to_string(expected)); }
void Contracts(Fixture& f) {
    auto current=Solid({.25f,.5f,.75f,.375f});
    // RGB neighbourhood extrema deliberately span history without spatially blurring centre.
    for(unsigned y=0;y<Size;++y) for(unsigned x=0;x<Size;++x) if((x+y)%2) current[y*Size+x]={1,0,1,.625f};
    auto history=Solid({.75f,.25f,.875f,0}), motion=Solid({0,0,.4f,0}), depth=Solid({.4f,0,0,0});
    Constants c; c.extentValidity[2]=0; Exact(f.Draw(current,history,motion,depth,c),current,"global reset"); c.extentValidity[2]=1;
    auto out=f.Draw(current,history,motion,depth,c); const unsigned p=16*Size+16;
    Near(out[p][0],.65f,"bounded accumulation"); Near(out[p][1],.3f,"RGB accumulation");
    for(unsigned i=0;i<out.size();++i) Near(out[i][3],current[i][3],"current alpha");
    c.accumulation[0]=10; out=f.Draw(current,history,motion,depth,c); Near(out[p][0],.725f,"weight upper bound");
    c.accumulation[0]=-1; Exact(f.Draw(current,history,motion,depth,c),current,"negative weight"); c.accumulation[0]=.8f;
    for(unsigned component=0;component<2;++component) {
        auto invalid=c; invalid.accumulation[component]=std::numeric_limits<float>::quiet_NaN();
        Exact(f.Draw(current,history,motion,depth,invalid),current,"nonfinite parameters");
    }
    c.accumulation[1]=.002f;
    out=f.Draw(current,history,motion,Solid({.401f,0,0,0}),c); Near(out[p][0],.65f,"depth tolerance acceptance");
    c.accumulation[1]=1;
    Exact(f.Draw(current,history,motion,Solid({.42f,0,0,0}),c),current,"depth tolerance upper bound");
    c.accumulation[1]=-1;
    Exact(f.Draw(current,history,motion,Solid({.4001f,0,0,0}),c),current,"depth tolerance lower bound");
    c.accumulation[1]=.001f;
    for(Pixel invalid: {Pixel{0,0,-1,0},Pixel{0,0,1.1f,0},Pixel{0,0,.4f,1},Pixel{2,0,.4f,0},Pixel{0,2,.4f,0},Pixel{0,0,.4f,-1},Pixel{0,0,.4f,2}})
        Exact(f.Draw(current,history,Solid(invalid),depth,c),current,"invalid/reactive/out-of-image");
    for(unsigned component=0;component<4;++component) for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        Pixel m{0,0,.4f,0}; m[component]=invalid; Exact(f.Draw(current,history,Solid(m),depth,c),current,"nonfinite motion");
    }
    for(float invalid:{.6f,-1.f,1.1f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        Exact(f.Draw(current,history,motion,Solid({invalid,0,0,0}),c),current,"invalid/disoccluded previous depth");
    out=f.Draw(current,history,Solid({0,0,.4f,.5f}),depth,c); Near(out[p][0],.45f,"partial reactivity");
    // Previous z=.4 is accepted regardless of hypothetical current z=.8: current
    // depth intentionally has no binding. Camera movement must use motion.z.
    c.previousViewport={8,8,24,24}; out=f.Draw(current,history,motion,depth,c);
    Check(out[0]==current[0],"previous viewport rejection"); Near(out[p][0],.65f,"previous viewport interior");
    c.viewport={8,8,24,24}; out=f.Draw(current,history,motion,depth,c); Check(out[0]==current[0],"outside scene bypass");
    c=Constants{};
    out=f.Draw(Solid({.25f,.5f,.75f,.375f}),history,motion,depth,c); Exact(out,Solid({.25f,.5f,.75f,.375f}),"3x3 clamp");
    auto neighbourhood=Solid({.25f,.5f,.75f,.375f});
    neighbourhood[p-Size-1]={1,0,1,.375f}; // Diagonal belongs to the 3x3, not a cross.
    out=f.Draw(neighbourhood,history,motion,depth,c); Near(out[p][0],.65f,"3x3 diagonal extrema");
    neighbourhood[p-Size-1]=neighbourhood[p]; neighbourhood[p-2]={1,0,1,.375f};
    out=f.Draw(neighbourhood,history,motion,depth,c); Check(out[p]==neighbourhood[p],"3x3 must not include distant extrema");
    // Nonzero fractional velocity verifies top-down sign and all four depth taps.
    auto gradient=history;
    for(unsigned y=0;y<Size;++y) for(unsigned x=0;x<Size;++x) gradient[y*Size+x]={x/32.f,y/32.f,.8f,0};
    auto moving=Solid({.5f/Size,.25f/Size,.4f,0});
    out=f.Draw(current,gradient,moving,depth,c);
    Near(out[p][0],.25f*.2f+(15.5f/32)*.8f,"bilinear x reprojection");
    Near(out[p][1],.5f*.2f+(15.75f/32)*.8f,"top-down y reprojection");
    for(unsigned y=15;y<=16;++y) for(unsigned x=15;x<=16;++x) {
        auto badDepth=depth; badDepth[y*Size+x][0]=.6f;
        out=f.Draw(current,gradient,moving,badDepth,c); Check(out[p]==current[p],"disoccluded bilinear tap leaked");
    }
    c.previousViewport={16,16,24,24}; out=f.Draw(current,gradient,moving,depth,c); Check(out[p]==current[p],"outside viewport bilinear tap leaked");
    auto badHistory=history; badHistory[p][0]=std::numeric_limits<float>::quiet_NaN();
    c=Constants{}; out=f.Draw(current,badHistory,motion,depth,c); Check(out[p]==current[p],"nonfinite history rejection");
    // HDR/negative numeric values must not be gamma transformed or saturated.
    auto numeric=current; for(auto& pixel:numeric) for(unsigned k=0;k<3;++k) pixel[k]=pixel[k]*4-2;
    c.extentValidity[2]=0; Exact(f.Draw(numeric,history,motion,depth,c),numeric,"classic numeric representation");
    std::cout << "PASS: reset, alpha, bounds, finite checks, expected previous depth, all bilinear taps, clamp, reactivity, weight bounds, numeric space\n";
}
float Radical(unsigned i,unsigned base) { float value=0,scale=1; while(i) { scale/=base; value+=(i%base)*scale; i/=base; } return value; }
bool Edge(float x,float y) { return x > 7.37f+.43f*y; }
void Quality(Fixture& f) {
    // Actual 64-frame jittered point-sampled raster-edge inputs. Static geometry:
    // true unjittered velocity is zero, not fabricated jitter motion. GPU-resolved
    // output is read back and fed into the next frame's GPU history texture.
    Pixels reference(Size*Size),history=Solid({0,0,0,1});
    for(unsigned y=0;y<Size;++y) for(unsigned x=0;x<Size;++x) {
        float sum=0; for(unsigned sy=0;sy<64;++sy) for(unsigned sx=0;sx<64;++sx) sum+=Edge(x+(sx+.5f)/64,y+(sy+.5f)/64)?1.f:0.f;
        reference[y*Size+x]={sum/4096,sum/4096,sum/4096,1};
    }
    const auto motion=Solid({0,0,.5f,0}), depth=Solid({.5f,0,0,0});
    double offError=0,taaError=0; unsigned measured=0;
    for(unsigned frame=0;frame<64;++frame) {
        const float jx=Radical(frame%8+1,2)-57.f/128.f,jy=Radical(frame%8+1,3)-.5f;
        Pixels current(Size*Size);
        for(unsigned y=0;y<Size;++y) for(unsigned x=0;x<Size;++x) {
            float v=Edge(x+.5f+jx,y+.5f+jy)?1.f:0.f; current[y*Size+x]={v,v,v,.625f};
        }
        Constants c; c.extentValidity[2]=frame!=0; c.accumulation[0]=.875f;
        history=f.Draw(current,history,motion,depth,c);
        if(frame>=32) for(unsigned i=0;i<history.size();++i) {
            double a=current[i][0]-reference[i][0], b=history[i][0]-reference[i][0]; offError+=a*a; taaError+=b*b; ++measured;
            Near(history[i][3],.625f,"sequence alpha");
        }
    }
    offError/=measured; taaError/=measured;
    std::cout << "Subpixel edge: 64 frames, 8-sample Halton cycle, last32 MSE vs 64x64/pixel reference: AA-off=" << offError << " TAA=" << taaError << " ratio=" << taaError/offError << '\n';
    Check(taaError<offError*.75,"TAA must improve converged supersampled-reference error by >25%");
    std::cout << "PASS: synthetic static subpixel edge quality (not foliage/animation or game acceptance)\n";
}
}
int main() {
    try { Fixture fixture; fixture.Init(); Contracts(fixture); Quality(fixture); return 0; }
    catch(const Unsupported& error) { std::cout << "SKIP: " << error.what() << '\n'; return 77; }
    catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
