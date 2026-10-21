#include "Engine/Source/Gx/CGxDeviceVulkan/PresentMode.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        auto check=[](bool value) { if(!value) throw std::runtime_error("VSync presentation policy mismatch"); };
        using GxVulkan::SelectPresentMode;
        const VkPresentModeKHR all[]={VK_PRESENT_MODE_FIFO_KHR,VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_IMMEDIATE_KHR};
        const VkPresentModeKHR mailbox[]={VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_FIFO_KHR};
        const VkPresentModeKHR fifo[]={VK_PRESENT_MODE_FIFO_KHR};
        const VkPresentModeKHR relaxed[]={VK_PRESENT_MODE_FIFO_RELAXED_KHR,VK_PRESENT_MODE_FIFO_KHR};
        check(SelectPresentMode(true,all,3)==VK_PRESENT_MODE_FIFO_KHR);
        check(SelectPresentMode(false,all,3)==VK_PRESENT_MODE_IMMEDIATE_KHR);
        check(SelectPresentMode(false,mailbox,2)==VK_PRESENT_MODE_MAILBOX_KHR);
        check(SelectPresentMode(true,mailbox,2)==VK_PRESENT_MODE_FIFO_KHR);
        check(SelectPresentMode(false,fifo,1)==VK_PRESENT_MODE_FIFO_KHR);
        check(SelectPresentMode(false,relaxed,2)==VK_PRESENT_MODE_FIFO_KHR);
        check(SelectPresentMode(false,nullptr,0)==VK_PRESENT_MODE_FIFO_KHR);
        CGxFormat defaults;
        check(defaults.m_vsync==0);
        std::cout<<"VSync on selects FIFO; off selects supported immediate/mailbox, FIFO fallback; defined default\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
