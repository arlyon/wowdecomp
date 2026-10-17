#if !defined(_WIN32)

#include "Engine/Source/SteelseriesMouse/HidManager.h"
#include <cerrno>

namespace {
// No specialty HID backend is installed. Interface ownership is not device presence.
class UnavailableHidManager final : public HidManager {
public:
    bool EnumerateDevices() override { errno = ENOTSUP; return false; }
    void SetWindow(void*) override {}
    uint32_t GetDeviceCount() override { return 0; }
    bool WriteWait(const void*, uint32_t) override { errno = ENOTSUP; return false; }
    bool Write(const void*, uint32_t) override { errno = ENOTSUP; return false; }
    bool SetFeature(void*, uint32_t) override { errno = ENOTSUP; return false; }
};
}

HidManager::~HidManager() = default;

HidManager* HidManager::Create() {
    return new UnavailableHidManager();
}

#endif
