#include "Engine/Source/Gx/CGxDeviceVulkan/RecordingResources.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Vk(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Recording fixture VkResult " + std::to_string(result));
}
static_assert(!std::is_copy_constructible_v<GxVulkan::RecordingResources>);
static_assert(!std::is_move_constructible_v<GxVulkan::RecordingResources>);

struct Fixture {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkSemaphore gate{};
    bool timeline = false;
    uint32_t family = 0;
    std::mutex queueMutex;
    GxVulkan::RecordingResources world, ui;

    ~Fixture() {
        if (device) {
            // A test failure must not strand its own application-controlled wait.
            if (gate) {
                VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
                vkSignalSemaphore(device, &signal);
            }
            vkDeviceWaitIdle(device);
            world.Destroy(); ui.Destroy();
            if (gate) vkDestroySemaphore(device, gate, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void Init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
        Vk(vkCreateInstance(&info, nullptr, &instance));
        uint32_t count = 0; Vk(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        Check(count != 0, "No physical device for recording fixture");
        std::vector<VkPhysicalDevice> devices(count); Vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        bool found = false;
        std::vector<VkQueueFamilyProperties> queues;
        for (auto candidate : devices) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr); queues.resize(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
            for (uint32_t i = 0; i < count; ++i) {
                if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    physical = candidate; family = i; found = true; break;
                }
            }
            if (found) break;
        }
        Check(found, "No graphics queue for recording fixture");
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkPhysicalDeviceVulkan12Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        timeline = properties.apiVersion >= VK_API_VERSION_1_2 && supported.timelineSemaphore;
        VkPhysicalDeviceVulkan12Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled.timelineSemaphore = timeline;
        const float priority = 1.f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queueInfo;
        if (timeline) create.pNext = &enabled;
        Vk(vkCreateDevice(physical, &create, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        world.Initialize(device, family, memory, 4096); ui.Initialize(device, family, memory, 4096);
        Check(world.commandPool != ui.commandPool && world.command != ui.command && world.fence != ui.fence &&
            world.descriptorPool != ui.descriptorPool && world.upload.buffer != ui.upload.buffer &&
            world.upload.memory != ui.upload.memory, "Recording lanes share transient resources");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptorInfo.bindingCount = 1; descriptorInfo.pBindings = &binding;
        Vk(vkCreateDescriptorSetLayout(device, &descriptorInfo, nullptr, &descriptors));
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1; pipelineInfo.pSetLayouts = &descriptors;
        Vk(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &layout));
        std::cout << "Recording topology: device=" << properties.deviceName << " logicalDevices=1 queueFamily=" << family
            << " familyQueues=" << queues[family].queueCount << " requestedQueues=1 timeline=" << timeline << '\n';
    }

    VkDescriptorSet Begin(GxVulkan::RecordingResources& lane, uint32_t value) {
        Check(lane.TryReset(), "Completed lane refused reuse");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = lane.descriptorPool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &descriptors;
        VkDescriptorSet set{}; Vk(vkAllocateDescriptorSets(device, &allocate, &set));
        VkDescriptorBufferInfo buffer{lane.upload.buffer, 0, 16};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        lane.uploadOffset = 16;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Vk(vkBeginCommandBuffer(lane.command, &begin));
        vkCmdBindDescriptorSets(lane.command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdFillBuffer(lane.command, lane.upload.buffer, 0, 16, value);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(lane.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        Vk(vkEndCommandBuffer(lane.command));
        return set;
    }

    void Submit(GxVulkan::RecordingResources& lane, bool gated = false) {
        Vk(vkResetFences(device, 1, &lane.fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &lane.command;
        const uint64_t value = 1;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        if (gated) {
            timelineInfo.waitSemaphoreValueCount = 1; timelineInfo.pWaitSemaphoreValues = &value;
            submit.pNext = &timelineInfo; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &gate; submit.pWaitDstStageMask = &stage;
        }
        std::lock_guard<std::mutex> lock(queueMutex);
        Vk(vkQueueSubmit(queue, 1, &submit, lane.fence));
    }

    void Wait(GxVulkan::RecordingResources& lane, uint32_t expected) {
        Vk(vkWaitForFences(device, 1, &lane.fence, VK_TRUE, 5'000'000'000ull));
        Check(lane.Complete(), "Completed fence not observed");
        uint32_t output[4]{}; std::memcpy(output, lane.upload.mapped, sizeof(output));
        for (auto value : output) Check(value == expected, "Recording lane output corrupted");
    }

    void CpuHold() {
        std::mutex mutex; std::condition_variable changed;
        bool entered = false, release = false;
        std::exception_ptr error;
        const uint32_t worldValue = 0x1234ABCD;
        std::thread producer([&] {
            try {
                Begin(world, worldValue);
                {
                    std::unique_lock<std::mutex> lock(mutex); entered = true; changed.notify_all();
                    changed.wait(lock, [&] { return release; });
                }
                Submit(world);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex); error = std::current_exception(); entered = true; changed.notify_all();
            }
        });
        auto join = [&] {
            { std::lock_guard<std::mutex> lock(mutex); release = true; changed.notify_all(); }
            producer.join();
        };
        try {
            {
                std::unique_lock<std::mutex> lock(mutex);
                Check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "Producer did not enter CPU hold");
                if (error) std::rethrow_exception(error);
            }
            const auto started = std::chrono::steady_clock::now();
            for (uint32_t i = 1; i <= 16; ++i) {
                Begin(ui, i); Submit(ui); Wait(ui, i);
                Check(world.uploadOffset == 16, "UI reset the producer's upload arena");
            }
            std::this_thread::sleep_until(started + std::chrono::milliseconds(500));
            join();
            if (error) std::rethrow_exception(error);
            Wait(world, worldValue);
            std::cout << "Separate recording: 16 UI-lane completions during a >=500ms pre-submit producer hold; world descriptors/output retained\n";
        } catch (...) { if (producer.joinable()) join(); throw; }
    }

    void PendingReuse() {
        if (!timeline) {
            std::cout << "Pending-GPU reuse test unavailable: timeline semaphore not supported\n";
            return;
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; create.pNext = &type;
        Vk(vkCreateSemaphore(device, &create, nullptr, &gate));
        Begin(world, 42); Submit(world, true);
        Check(!world.Complete(), "Gated producer unexpectedly complete");
        Check(!world.TryReset() && world.uploadOffset == 16, "Pending producer storage was reset");
        // Do not enqueue UI behind this wait. This negative case deliberately
        // demonstrates the shared-queue head-of-line limit, not GPU independence.
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
        Vk(vkSignalSemaphore(device, &signal)); Wait(world, 42);
        vkDestroySemaphore(device, gate, nullptr); gate = VK_NULL_HANDLE;
        Check(world.TryReset() && world.uploadOffset == 0, "Completed producer arena did not reset");
        std::cout << "Pending reuse: nonblocking rejection until actual GPU completion passed\n";
    }

    void Lifecycle() {
        world.Destroy(); world.Destroy();
        Check(!world.commandPool && !world.command && !world.fence && !world.descriptorPool &&
            !world.upload.buffer && !world.upload.memory && !world.uploadOffset, "Destroyed lane retained handles");
        world.Initialize(device, family, memory, 4096);
        Begin(world, 73); Submit(world); Wait(world, 73);
        bool rejected = false;
        try { world.Initialize(device, family, memory, 4096); } catch (const std::logic_error&) { rejected = true; }
        Check(rejected, "Live lane reinitialization was not rejected");
        GxVulkan::RecordingResources empty;
        rejected = false;
        try { empty.TryReset(); } catch (const std::logic_error&) { rejected = true; }
        Check(rejected, "Uninitialized lane reuse was not rejected");
        auto unavailable = memory; unavailable.memoryTypeCount = 0;
        rejected = false;
        try { empty.Initialize(device, family, unavailable, 4096); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && !empty.commandPool && !empty.command && !empty.fence &&
            !empty.descriptorPool && !empty.upload.buffer, "Partial initialization leaked recording resources");
        empty.Initialize(device, family, memory, 4096);
        Check(empty.Complete(), "Failed initialization prevented retry");
        std::cout << "Lifecycle: idempotent teardown, reinitialization, partial-allocation rollback and invalid-state rejection passed\n";
    }
};
}
int main() {
    try {
        Fixture fixture; fixture.Init(); fixture.CpuHold(); fixture.PendingReuse(); fixture.Lifecycle();
        std::cout << "Recording-resource isolation passed (not an original-UI cadence or presentation test)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
