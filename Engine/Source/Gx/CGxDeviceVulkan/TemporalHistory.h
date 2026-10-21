#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_TEMPORALHISTORY_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_TEMPORALHISTORY_H

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace GxVulkan {
struct TemporalFrameDescription {
    uint64_t frame = 0, scene = 0;
    uint32_t width = 0, height = 0;
    // Output-image pixels, not UI coordinates or a saved resolution setting.
    std::array<uint32_t,4> viewport{};
    bool enabled = true, cameraCut = false;
};
struct TemporalFrameState {
    bool active = false, historyValid = false;
    uint32_t sample = 0;
    std::array<float,2> jitterPixels{}, jitterClip{};
};

inline std::array<float,2> TemporalJitterPixels(uint32_t sample) {
    const auto radicalInverse = [](uint32_t i, uint32_t base) {
        float value = 0, scale = 1.f;
        while (i) { scale /= base; value += (i % base)*scale; i /= base; }
        return value;
    };
    const uint32_t index = sample % 8 + 1;
    // Re-centre the finite eight-sample Halton cycle, not just its infinite limit.
    return {radicalInverse(index,2)-57.f/128.f,radicalInverse(index,3)-.5f};
}

// Commit only after successful submission of the history-producing work; Cancel
// on failure. Aborted work, UI-only gaps and invalidation cannot promote stale data.
// This owns CPU validity/jitter state, not images or submission fences.
class TemporalHistory {
public:
    TemporalFrameState Begin(const TemporalFrameDescription& frame) {
        if (m_pending) throw std::logic_error("Temporal frame already pending");
        if (!frame.enabled || !frame.width || !frame.height ||
            !frame.viewport[2] || !frame.viewport[3]) {
            Cancel(); return {};
        }
        const auto& rect = frame.viewport;
        if (!frame.frame || rect[0] > frame.width || rect[1] > frame.height ||
            rect[2] > frame.width-rect[0] || rect[3] > frame.height-rect[1])
            throw std::invalid_argument("Invalid temporal frame bounds or ID");
        const bool valid = m_ready && !frame.cameraCut &&
            m_previous.frame != std::numeric_limits<uint64_t>::max() &&
            frame.frame == m_previous.frame+1 && frame.scene == m_previous.scene &&
            frame.width == m_previous.width && frame.height == m_previous.height &&
            frame.viewport == m_previous.viewport;
        m_current = frame; m_pending = true;
        m_currentSample = valid ? (m_previousSample+1)%8 : 0;
        const auto jitter = TemporalJitterPixels(m_currentSample);
        return {true,valid,m_currentSample,jitter,
            {2*jitter[0]/rect[2],-2*jitter[1]/rect[3]}};
    }
    void Commit() {
        if (!m_pending) throw std::logic_error("No temporal frame to commit");
        m_previous = m_current; m_previousSample = m_currentSample;
        m_ready = true; m_pending = false;
    }
    void Cancel() {
        m_pending = false; m_ready = false;
    }
private:
    TemporalFrameDescription m_current{}, m_previous{};
    uint32_t m_currentSample = 0, m_previousSample = 0;
    bool m_pending = false, m_ready = false;
};
}
#endif
