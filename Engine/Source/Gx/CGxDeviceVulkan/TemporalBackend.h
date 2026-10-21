#ifndef GX_VULKAN_TEMPORAL_BACKEND_H
#define GX_VULKAN_TEMPORAL_BACKEND_H

#include <map>
#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace GxVulkan {
// Reduced scenes need more phases than native TAA to cover the output grid.
// Keep the finite sequence centred; advance only on a submitted history producer.
inline std::array<float,2> TemporalUpscaleJitter(uint32_t sample) {
    static const auto sequence = [] {
        std::array<std::array<float,2>,32> values{};
        std::array<float,2> mean{};
        for (uint32_t i=0;i<values.size();++i) for (unsigned axis=0;axis<2;++axis) {
            const uint32_t base=axis ? 3 : 2;
            uint32_t n=i+1; float f=1, value=0;
            while (n) { f/=base; value+=f*(n%base); n/=base; }
            values[i][axis]=value; mean[axis]+=value/values.size();
        }
        for (auto& value:values) for (unsigned axis=0;axis<2;++axis) value[axis]-=mean[axis];
        return values;
    }();
    return sequence[sample%sequence.size()];
}

// Keep the shared recovered-shader ABI intact. Only motion varying xy is
// converted from viewport NDC to image NDC; raster position and depth are untouched.
inline std::string TemporalViewportSource(std::string source) {
    const std::string anchor = "    output.gx_previousClip = previous.";
    const auto start = source.find(anchor);
    const auto end = source.find(';', start);
    if (start == std::string::npos || end == std::string::npos ||
        source.find(anchor, start + anchor.size()) != std::string::npos)
        throw std::runtime_error("Unexpected temporal vertex wrapper");
    source.insert(end + 1, "\n    output.gx_currentClip.xy *= gx_temporalJitter.zw;\n    output.gx_previousClip.xy *= gx_temporalJitter.zw;");
    return source;
}

// Two generations only: recorded draws never become previous poses until the
// history-producing submission succeeds. Repeated keys in a frame are ambiguous,
// not a last-writer-wins pose. Explicit owners are the first word of each key.
class TemporalDraws {
public:
    using Key = std::vector<uint64_t>;
    struct Pose {
        std::vector<float> constants;
        std::vector<const void*> buffers;
        float minDepth = 0, maxDepth = 1;
        bool valid = false;
    };
    const Pose* Previous(const Key& key) const {
        auto it = previous.find(key);
        return it != previous.end() && it->second.valid ? &it->second : nullptr;
    }
    bool Seen(const Key& key) const { return current.find(key) != current.end(); }
    bool Ambiguous() const { return ambiguous; }
    void Record(const Key& key, Pose pose) {
        auto result = current.emplace(key, std::move(pose));
        if (!result.second) {
            result.first->second.valid = false;
            ambiguous = true;
        }
    }
    void ForgetOwner(const void* owner) {
        const auto id = reinterpret_cast<uint64_t>(owner);
        Erase([&](const Key& key, const Pose&) { return key[0] == id; });
    }
    void ForgetBuffer(const void* buffer) {
        Erase([&](const Key&, const Pose& pose) {
            for (auto* dependency : pose.buffers) if (dependency == buffer) return true;
            return false;
        });
    }
    void Commit() { previous = std::move(current); current.clear(); ambiguous = false; }
    void Reset() { previous.clear(); current.clear(); ambiguous = false; }
private:
    template<class Predicate> void Erase(Predicate predicate) {
        for (auto* entries : {&previous, &current})
            for (auto i = entries->begin(); i != entries->end();)
                if (predicate(i->first, i->second)) i = entries->erase(i); else ++i;
    }
    std::map<Key, Pose> previous, current;
    bool ambiguous = false;
};
}
#endif
