#ifndef ENGINE_SOURCE_GX_COMPLETEDWORLD_H
#define ENGINE_SOURCE_GX_COMPLETEDWORLD_H

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace GxWorld {
struct WorldFrameDescription {
    uint64_t scene = 0;
    uint32_t width = 0, height = 0;
    // Top-down output pixels; only this rectangle has published colour/depth.
    std::array<uint32_t,4> viewport{};
    // Original C44Matrix element order, captured with the pixels, not at reuse.
    std::array<float,16> view{}, projection{};
    float renderScale=1;
    bool hdr=false,temporal=false;
};
struct WorldFrameLease {
    uint32_t slot = 0;
    uint64_t epoch = 0, generation = 0;
};
inline bool operator==(const WorldFrameLease& a, const WorldFrameLease& b) {
    return a.slot == b.slot && a.epoch == b.epoch && a.generation == b.generation;
}
struct WorldFrame {
    WorldFrameLease lease;
    WorldFrameDescription description;
};

// Owner-thread publication ledger; GPU work is never waited on here. Producer
// and consumer serials are separate timelines, even when one queue serves both.
// A lease identifies a specific image allocation generation, not a reusable slot.
class CompletedWorld {
public:
    static constexpr unsigned SlotCount = 3;
    enum class Stage { Available, Recording, Pending, Complete, Displayed, Retired };
    struct Slot {
        Stage stage = Stage::Available;
        WorldFrameLease lease;
        WorldFrameDescription frame;
        uint64_t producer = 0, consumer = 0;
    };

    void Configure(uint64_t scene, uint32_t width, uint32_t height) {
        if (!scene || !width || !height) throw std::invalid_argument("Invalid world presentation identity");
        if (scene == currentScene && width == currentWidth && height == currentHeight) return;
        Invalidate(); currentScene = scene; currentWidth = width; currentHeight = height;
    }
    // Device teardown/recreation only: all submitted work has finished and any
    // unsubmitted recording is being discarded. Keep identities monotonic.
    void ResetAfterIdle() {
        ++epoch; slots = {}; displayed.reset(); produced = consumed = 0;
        currentScene = 0; currentWidth = currentHeight = 0;
    }
    void Invalidate() {
        ++epoch; displayed.reset();
        for (auto& slot : slots) {
            if (slot.stage == Stage::Complete || slot.stage == Stage::Displayed) slot.stage = Stage::Retired;
        }
        Collect();
    }
    std::optional<WorldFrameLease> Begin(const WorldFrameDescription& frame) {
        Validate(frame);
        // One producer, no request queue. The caller retries with its newest
        // state rather than building a backlog behind a busy generation.
        for (const auto& slot : slots)
            if (slot.stage == Stage::Recording || slot.stage == Stage::Pending) return {};
        for (unsigned i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (slot.stage != Stage::Available) continue;
            slot = {}; slot.stage = Stage::Recording; slot.frame = frame;
            slot.lease = {i, epoch, ++generation};
            return slot.lease;
        }
        return {};
    }
    void Describe(WorldFrameLease lease, const WorldFrameDescription& frame) {
        auto& slot = Find(lease);
        if (slot.stage != Stage::Recording) throw std::logic_error("Cannot change submitted world metadata");
        Validate(frame);slot.frame=frame;
    }
    void Submit(WorldFrameLease lease, uint64_t producerSerial) {
        auto& slot = Find(lease);
        if (slot.stage != Stage::Recording || !producerSerial || producerSerial <= produced)
            throw std::logic_error("Invalid world publication submission");
        slot.producer = producerSerial; slot.stage = Stage::Pending;
    }
    // Only for a recording that was abandoned before submission. Submitted GPU
    // work must retire through its real completion serial, including cancellation.
    void Abort(WorldFrameLease lease) {
        auto& slot = Find(lease);
        if (slot.stage != Stage::Recording) throw std::logic_error("Cannot abort submitted world work");
        slot = {};
    }
    void Poll(uint64_t producerCompleted, uint64_t consumerCompleted) {
        if (producerCompleted < produced || consumerCompleted < consumed)
            throw std::logic_error("World completion timelines moved backwards");
        produced = producerCompleted; consumed = consumerCompleted;
        for (auto& slot : slots) {
            if (slot.stage != Stage::Pending || slot.producer > produced) continue;
            slot.stage = slot.lease.epoch == epoch ? Stage::Complete : Stage::Retired;
        }
        Collect();
    }
    std::optional<WorldFrameLease> Latest() {
        std::optional<WorldFrameLease> newest = displayed;
        for (const auto& slot : slots)
            if (slot.stage == Stage::Complete && slot.lease.epoch == epoch &&
                (!newest || slot.lease.generation > newest->generation)) newest = slot.lease;
        if (!newest) return {};
        if (displayed && !(*displayed == *newest)) Find(*displayed).stage = Stage::Retired;
        for (auto& slot : slots)
            if (slot.stage == Stage::Complete && !(slot.lease == *newest)) slot.stage = Stage::Retired;
        Find(*newest).stage = Stage::Displayed; displayed = newest;
        Collect(); return newest;
    }
    void Consume(WorldFrameLease lease, uint64_t consumerSerial) {
        auto& slot = Find(lease);
        if (slot.stage != Stage::Displayed || slot.lease.epoch != epoch ||
            !consumerSerial || consumerSerial <= consumed || consumerSerial < slot.consumer)
            throw std::logic_error("Invalid world presentation consumer");
        slot.consumer = consumerSerial;
    }
    const Slot& Get(WorldFrameLease lease) const {
        if (lease.slot >= slots.size() || !(slots[lease.slot].lease == lease) ||
            slots[lease.slot].stage == Stage::Available)
            throw std::logic_error("Stale world presentation lease");
        return slots[lease.slot];
    }
    Stage Status(unsigned slot) const { return slots.at(slot).stage; }

private:
    std::array<Slot,SlotCount> slots{};
    uint64_t epoch = 1, generation = 0, produced = 0, consumed = 0;
    uint64_t currentScene = 0;
    uint32_t currentWidth = 0, currentHeight = 0;
    std::optional<WorldFrameLease> displayed;

    Slot& Find(WorldFrameLease lease) {
        Get(lease); return slots[lease.slot];
    }
    void Validate(const WorldFrameDescription& frame) const {
        const auto& rect = frame.viewport;
        if (frame.scene != currentScene || frame.width != currentWidth || frame.height != currentHeight ||
            !frame.width || !frame.height || !rect[2] || !rect[3] || rect[0] > frame.width || rect[1] > frame.height ||
            rect[2] > frame.width-rect[0] || rect[3] > frame.height-rect[1])
            throw std::invalid_argument("Incompatible world publication bounds");
    }
    void Collect() {
        for (auto& slot : slots)
            if (slot.stage == Stage::Retired && slot.producer <= produced && slot.consumer <= consumed) slot = {};
    }
};
}
#endif
