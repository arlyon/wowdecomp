#ifndef GX_VULKAN_HDR_LUT_JOB_H
#define GX_VULKAN_HDR_LUT_JOB_H
#include "HdrLut.h"
#include "Engine/Source/Gx/HdrSettings.h"
#include <atomic>
#include <fstream>
#include <thread>
#include <filesystem>

namespace GxVulkan {
// One owned job; no engine/device references cross the worker boundary. The
// owner keeps Reservation charged until both the worker and its output drain.
class HdrLutJob {
public:
    static constexpr size_t Reservation = 33u*33u*33u*3u*sizeof(float) + 64u*1024u;
    static bool ValidPath(const std::string& path) {
        return GxHdrLutPathValid(path.c_str());
    }
    explicit HdrLutJob(std::string path) : worker([this,path=std::move(path)] {
        try {
            if(!ValidPath(path) || !std::filesystem::is_regular_file(path))
                throw std::runtime_error("LUT requires a local regular .cube file");
            std::ifstream stream(path);
            if(!stream) throw std::runtime_error("Cannot open HDR LUT");
            lut=ReadHdrCube(stream);
        } catch(const std::exception& e) { error=e.what(); }
        done.store(true, std::memory_order_release);
    }) {}
    ~HdrLutJob() { if(worker.joinable()) worker.join(); }
    bool Ready() const { return done.load(std::memory_order_acquire); }
    HdrLut lut;
    std::string error;
private:
    std::atomic<bool> done{false};
    std::thread worker;
};
}
#endif
