#include "Engine/Source/Gx/CompletedWorld.h"
#include <iostream>
#include <stdexcept>

namespace {
using GxWorld::CompletedWorld;
using GxWorld::WorldFrameDescription;
void Check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class Action> void Reject(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "Invalid world transition accepted");
}
WorldFrameDescription Frame(uint64_t scene = 7) {
    WorldFrameDescription result; result.scene = scene; result.width = 640; result.height = 480;
    result.viewport = {80,60,480,360}; result.view[12] = 123; result.projection[0] = 2;
    return result;
}
void CompletionAndReuse() {
    CompletedWorld world; world.Configure(7,640,480);
    Check(!world.Latest(), "Uninitialized image was published");
    auto description = Frame(); const auto first = world.Begin(description);
    Check(bool(first), "First generation refused");
    description.view[12] = 999;
    Check(!world.Begin(description), "More than one producer was admitted");
    Check(!world.Latest(), "Recording image was published");
    world.Submit(*first,3);
    Check(!world.Latest() && !world.Begin(description), "Pending image was exposed or producer queue grew");
    world.Poll(2,0); Check(!world.Latest(), "Incomplete producer serial was published");
    world.Poll(3,0); Check(world.Latest() == first, "Completed image was not published");
    for (uint64_t frame = 1; frame <= 100; ++frame) {
        Check(world.Latest() == first, "UI-only frame advanced the world generation");
        world.Consume(*first,frame); world.Poll(3,frame);
        Check(world.Get(*first).frame.view[12] == 123 && world.Get(*first).frame.projection[0] == 2,
            "Displayed image metadata followed the mutable camera");
    }
    const auto second = world.Begin(description); Check(bool(second), "Next generation refused");
    world.Submit(*second,4);
    Check(world.Latest() == first, "Incomplete new image replaced completed output");
    world.Poll(4,100); Check(world.Latest() == second, "Newest complete generation not selected");
    Check(world.Get(*second).frame.view[12] == 999, "New camera metadata did not follow new pixels");
    Reject([&] { world.Get(*first); });
}
void ConsumerRetentionAndBounds() {
    CompletedWorld world; world.Configure(7,640,480);
    auto first = *world.Begin(Frame()); world.Submit(first,1); world.Poll(1,0); world.Latest(); world.Consume(first,10);
    auto second = *world.Begin(Frame()); world.Submit(second,2); world.Poll(2,0); world.Latest(); world.Consume(second,11);
    Check(world.Status(first.slot) == CompletedWorld::Stage::Retired, "Old image with a reader was overwritten");
    auto third = *world.Begin(Frame()); world.Submit(third,3); world.Poll(3,0); world.Latest(); world.Consume(third,12);
    Check(!world.Begin(Frame()), "Three pinned slots did not bound memory");
    world.Poll(3,9); Check(!world.Begin(Frame()), "Consumer completion was guessed from producer completion");
    world.Poll(3,10);
    auto fourth = world.Begin(Frame()); Check(fourth && fourth->slot == first.slot, "Completed reader did not release its image slot");
    Check(fourth->generation != first.generation, "Slot reuse recycled the image identity");
    Reject([&] { world.Consume(first,13); });
    world.Abort(*fourth);
}
void InvalidationAndFailure() {
    CompletedWorld world; world.Configure(7,640,480);
    const auto pending = *world.Begin(Frame()); world.Submit(pending,1);
    world.Configure(8,640,480); world.Poll(1,0);
    Check(!world.Latest(), "Old scene reappeared after completion");
    Reject([&] { world.Get(pending); });
    auto recording = *world.Begin(Frame(8));
    world.Invalidate(); Check(!world.Begin(Frame(8)), "Canceled but still recording resources were reused");
    world.Abort(recording); Check(bool(world.Begin(Frame(8))), "Abandoned unsubmitted generation leaked a slot");

    CompletedWorld resized; resized.Configure(7,640,480);
    auto displayed = *resized.Begin(Frame()); resized.Submit(displayed,1); resized.Poll(1,0); resized.Latest(); resized.Consume(displayed,2);
    resized.Configure(7,320,240); Check(!resized.Latest(), "Wrong-size output remained displayable");
    auto small = Frame(); small.width = 320; small.height = 240; small.viewport = {0,0,320,240};
    auto replacement = resized.Begin(small); Check(bool(replacement), "Resize could not use another free slot");
    Check(replacement->slot != displayed.slot, "Resize overwrote an in-flight UI source");
    resized.Abort(*replacement); resized.Poll(1,2);
    Reject([&] { resized.Get(displayed); });
    Reject([&] { resized.Begin(Frame()); });
    Reject([&] { resized.Poll(0,2); });
    auto invalid = small; invalid.viewport[2] = 321;
    Reject([&] { resized.Begin(invalid); });
    auto abandoned = *resized.Begin(small); resized.Abort(abandoned);
    Reject([&] { resized.Submit(abandoned,3); });
    const auto old = *resized.Begin(small);
    resized.ResetAfterIdle(); resized.Configure(7,320,240);
    const auto fresh = *resized.Begin(small);
    Check(fresh.epoch != old.epoch && fresh.generation > old.generation,
        "Device/resize reset recycled a published identity");
    Reject([&] { resized.Submit(old,3); });
    resized.Abort(fresh);
}
}
int main() {
    try {
        CompletionAndReuse(); ConsumerRetentionAndBounds(); InvalidationAndFailure();
        std::cout << "Completed world: latest-completed reuse, paired camera metadata, bounded slots, separate reader completion, invalidation and failure passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
