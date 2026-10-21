#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_WORLDSUBMISSION_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_WORLDSUBMISSION_H
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace GxVulkan {
// The worker receives only a finished command buffer and a completion fence.
// It never calls the game, SDL, Lua, material callbacks or mutable Gx state.
// Dispatch follows the UI submission containing immutable input uploads/prefix.
class WorldSubmission {
public:
    WorldSubmission(VkQueue queue, std::mutex& queueMutex):queue(queue),queueMutex(queueMutex),thread([this]{Run();}) {}
    ~WorldSubmission(){Stop();}
    WorldSubmission(const WorldSubmission&)=delete;
    WorldSubmission& operator=(const WorldSubmission&)=delete;
    void Dispatch(VkCommandBuffer command, VkFence fence) {
        std::lock_guard<std::mutex> lock(control);
        if(stopping || pending)throw std::logic_error("World submitter already has work");
        this->command=command;this->fence=fence;result.store(VK_NOT_READY,std::memory_order_release);
        pending=true;changed.notify_all();
    }
    VkResult Result() const{return result.load(std::memory_order_acquire);}
    bool Held(){std::lock_guard<std::mutex> lock(control);return pending&&hold&&!executing;}
    void Hold(bool enabled) {
        std::lock_guard<std::mutex> lock(control);hold=enabled;changed.notify_all();
    }
    void FlushSubmission() {
        std::unique_lock<std::mutex> lock(control);hold=false;changed.notify_all();
        changed.wait(lock,[&]{return !pending || stopping;});
    }
    void Stop() {
        {std::lock_guard<std::mutex> lock(control);stopping=true;hold=false;changed.notify_all();}
        if(thread.joinable())thread.join();
    }
private:
    VkQueue queue;
    std::mutex& queueMutex;
    std::mutex control;
    std::condition_variable changed;
    bool stopping=false,pending=false,hold=false,executing=false;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    std::atomic<VkResult> result{VK_NOT_READY};
    std::thread thread;
    void Run() {
        std::unique_lock<std::mutex> lock(control);
        for(;;) {
            changed.wait(lock,[&]{return stopping || (pending&&!hold);});
            if(stopping)return;
            const auto buffer=command;const auto completion=fence;executing=true;
            lock.unlock();
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&buffer;
            VkResult submitted;
            {std::lock_guard<std::mutex> queueLock(queueMutex);submitted=vkQueueSubmit(queue,1,&submit,completion);}
            lock.lock();pending=false;executing=false;result.store(submitted,std::memory_order_release);changed.notify_all();
        }
    }
};
}
#endif
