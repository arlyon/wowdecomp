#ifndef GX_VULKAN_PRESENTMODE_H
#define GX_VULKAN_PRESENTMODE_H
#include <cstddef>
#include <initializer_list>
#include <vulkan/vulkan.h>

namespace GxVulkan {
inline VkPresentModeKHR SelectPresentMode(bool vsync,const VkPresentModeKHR* modes,size_t count) {
    if(!vsync) {
        for(auto preferred:{VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_MAILBOX_KHR})
            for(size_t i=0;i<count;++i) if(modes[i]==preferred) return preferred;
    }
    // FIFO is required by Vulkan; unsupported nonblocking modes must not be used.
    return VK_PRESENT_MODE_FIFO_KHR;
}
}
#endif
