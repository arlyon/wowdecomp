#ifndef GX_VULKAN_GPU_ADMISSION_H
#define GX_VULKAN_GPU_ADMISSION_H
#include <cstdint>
#include <functional>
#include <exception>
#include <stdexcept>

namespace GxVulkan {
// Render-thread physical allocation ledger. Reservations include temporary,
// recorded, submitted and retired storage until the owner actually frees it.
// This ledger does not own CPU payloads or perform asset/worker operations.
class GpuAdmission {
public:
    explicit GpuAdmission(uint64_t limit, uint64_t farLimit = 32ull * 1024 * 1024)
        : m_limit(limit), m_farLimit(farLimit) {}
    GpuAdmission(const GpuAdmission&) = delete;
    GpuAdmission& operator=(const GpuAdmission&) = delete;
    bool Charge(uint64_t bytes, bool far) {
        if (!bytes || bytes > m_limit || (far && (bytes > m_farLimit || m_far > m_farLimit - bytes))) return false;
        if (!far && bytes > m_limit - Used() && withdrawFar) withdrawFar();
        if (bytes > m_limit - Used()) return false;
        (far ? m_far : m_near) += bytes;
        return true;
    }
    void Release(uint64_t bytes, bool far) noexcept {
        auto& used = far ? m_far : m_near;
        if (bytes > used) std::terminate();
        used -= bytes;
    }
    uint64_t Used() const { return m_near + m_far; }
    uint64_t Far() const { return m_far; }
#ifdef WOWDECOMP_VULKAN_TESTING
    void SetEmptyLimitsForTesting(uint64_t limit,uint64_t farLimit) {
        if(Used()) throw std::logic_error("Admission test limits require an empty ledger");
        m_limit=limit;m_farLimit=farLimit;
    }
#endif
    std::function<void()> withdrawFar;
private:
    uint64_t m_limit, m_farLimit;
    uint64_t m_near = 0, m_far = 0;
};
}
#endif
