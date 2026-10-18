#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
// The fixture constructs the actual private scheduler state, not subsystem startup.
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Native/Streaming.cpp"

namespace {
namespace Lock = Blizzard::Lock;
using namespace Blizzard::Streaming;

void Check(bool value) {
    if (!value) {
        std::fputs("StreamingQueues assertion failed\n", stderr);
        std::abort();
    }
}

struct Fixture {
    std::deque<FILE_THREAD_CMD> queues[7];
    DigestStateMap digests;
    Fixture() {
        Lock::MutexCreate(s_queueLock);
        Lock::EventCreate(s_queueEvent);
        for (int i = 0; i < 7; ++i) s_queues[i] = &queues[i];
        s_digestStates = &digests;
        s_queueRotation[0] = s_queueRotation[1] = 0;
        s_useBackgroundQueues = true;
    }
    ~Fixture() {
        for (auto& queue : s_queues) queue = nullptr;
        s_digestStates = nullptr;
        Lock::EventDestroy(s_queueEvent);
        Lock::MutexDestroy(s_queueLock);
    }
    void Push(int priority, int type = 1, Completion* completion = nullptr) {
        Lock::ScopedMutex lock(s_queueLock);
        queues[priority].push_back({nullptr, type, nullptr, nullptr, completion});
    }
};

struct Signal {
    Lock::Event event;
    Completion completion;
    Signal() : completion{&event, 123, false, true} { Lock::EventCreate(event); }
    ~Signal() { Lock::EventDestroy(event); }
    bool Signaled() { return Lock::EventWait(0, event) == &event; }
};

void CompletionContract() {
    Fixture fixture;
    Signal signal;
    Check(!QueueCompletion(&signal.completion, 2));
    Check(signal.completion.m_result == 0 && !signal.completion.m_done && !signal.completion.m_success);
    Check(!signal.Signaled() && !Lock::EventWait(0, s_queueEvent));
    Check(!RemoveCompletion(&signal.completion, 2));
    signal.completion = {&signal.event, 123, false, true};
    fixture.Push(2);
    Check(QueueCompletion(&signal.completion, 2));
    Check(Lock::EventWait(0, s_queueEvent) == &s_queueEvent);
    Check(signal.completion.m_result == 123 && !signal.completion.m_done && signal.completion.m_success);
    auto& queued = fixture.queues[2].back();
    Check(queued.m_type == 2 && queued.m_completion == &signal.completion);
    Check(!queued.m_file && !queued.m_range && !queued.m_fs);
    Blizzard::Util::Digest<16> digest{};
    fixture.digests[digest] = {3, 2};
    digest.m_data[0] = 1;
    fixture.digests[digest] = {4, 1};
    Check(CancelQueue(2));
    Check(signal.Signaled() && !signal.Signaled()); // native event is auto-reset
    Check(signal.completion.m_result == 2 && signal.completion.m_done && signal.completion.m_success);
    Check(fixture.queues[2].size() == 2);
    for (const auto& command : fixture.queues[2]) Check(command.m_type == 0 && !command.m_completion);
    digest.m_data[0] = 0;
    Check(fixture.digests[digest].m_state == 0);
    digest.m_data[0] = 1;
    Check(fixture.digests[digest].m_state == 4);
    Check(CancelQueue(2) && !signal.Signaled());
    Check(CancelQueue(6));
    Signal attachedLoad;
    fixture.Push(4, 1, &attachedLoad.completion);
    Check(CancelQueue(4));
    Check(fixture.queues[4].front().m_type == 0);
    Check(fixture.queues[4].front().m_completion == &attachedLoad.completion);
    Check(!attachedLoad.Signaled() && !attachedLoad.completion.m_done);
}

void DetachContract() {
    Fixture fixture;
    Signal first, second;
    fixture.Push(0);
    Check(QueueCompletion(&first.completion, 0));
    Check(QueueCompletion(&second.completion, 0));
    Check(QueueCompletion(&first.completion, 0));
    Check(RemoveCompletion(&first.completion, 0));
    Check(fixture.queues[0].back().m_completion == nullptr);
    Check(fixture.queues[0][1].m_completion == &first.completion);
    Check(RemoveCompletion(&first.completion, 0));
    Check(!RemoveCompletion(&first.completion, 0));
    Check(CancelQueue(0));
    Check(!first.Signaled() && !first.completion.m_done && first.completion.m_result == 123);
    Check(second.Signaled() && second.completion.m_done && second.completion.m_result == 4);
    fixture.Push(3);
    Check(QueueCompletion(&first.completion, 3));
    FILE_THREAD_CMD command{};
    Check(PopCommand(&command, 3) && command.m_type == 1);
    Check(PopCommand(&command, 3) && command.m_type == 2);
    Check(command.m_completion == &first.completion);
    Check(!RemoveCompletion(&first.completion, 3));
    Check(CancelQueue(3) && !first.Signaled() && !first.completion.m_done);
    // Popped completions belong to the real worker; this fixture does not emulate it.
}

void SelectionContract() {
    Fixture fixture;
    FILE_THREAD_CMD command{};
    int queue = -1;
    Check(!PopCommand(&command, 0));
    Check(!SelectQueue(&command, 0, &queue));
    Check(queue == 1 && s_queueRotation[0] == 0);
    for (int group = 0; group < 2; ++group) {
        for (int i = 0; i < 3; ++i)
            for (int n = 0; n < 6; ++n) fixture.Push(group * 4 + i, 10 + n);
        int taken[3] = {};
        for (int i = 0; i < 6; ++i) {
            Check(SelectQueue(&command, group, &queue));
            Check(queue == group * 4 + s_queueOrder[i]);
            Check(command.m_type == 10 + taken[s_queueOrder[i]]++);
        }
        for (int i = 0; i < 3; ++i) fixture.queues[group * 4 + i].clear();
    }
    fixture.Push(5, 19);
    s_useBackgroundQueues = false;
    Check(!SelectQueue(&command, 1, &queue));
    s_useBackgroundQueues = true;
    Check(SelectQueue(&command, 1, &queue) && queue == 5 && command.m_type == 19);
    for (int i : {6, 3, 2}) fixture.Push(i, 30 + i);
    FILE_THREAD_CMD* next = nullptr;
    for (int i : {2, 3, 6}) {
        Check(GetNextCommand(&next, 0) == i);
        Check(next == s_currentCommands[0] && next->m_type == 30 + i);
        delete next;
        s_currentCommands[0] = nullptr;
    }
    next = nullptr;
    Check(GetNextCommand(&next, 1) == 7 && !next);
    fixture.Push(0, 41);
    fixture.Push(0, 42);
    Check(PopCommand(&command, 0) && command.m_type == 41);
    Check(PopCommand(&command, 0) && command.m_type == 42);
    Check(!PopCommand(&command, 0));
    Check(!CancelQueue(-1) && !CancelQueue(7));
    Check(!QueueCompletion(nullptr, 0) && !RemoveCompletion(nullptr, 0));
    Signal invalid;
    Check(!QueueCompletion(&invalid.completion, -1));
    Check(!RemoveCompletion(&invalid.completion, 7));
    Check(invalid.completion.m_result == 123 && !invalid.Signaled());
    Check(!PopCommand(nullptr, 0) && !PopCommand(&command, 7));
    Check(!SelectQueue(&command, 2, &queue) && !SelectQueue(&command, 0, nullptr));
    Check(GetNextCommand(&next, 2) == 7 && GetNextCommand(nullptr, 0) == 7);
    Lock::ScopedMutex outer(s_queueLock);
    fixture.Push(1);
    Check(PopCommand(&command, 1)); // external and helper recursive reentry
    Check(CancelQueue(1));
}

void WaiterContract() {
    Fixture fixture;
    Signal signal;
    fixture.Push(0);
    Check(QueueCompletion(&signal.completion, 0));
    std::thread waiter([&] {
        Lock::EventWaitForever(signal.event);
        Check(signal.completion.m_done && signal.completion.m_result == 2);
        Check(signal.completion.m_success);
    });
    Check(CancelQueue(0));
    waiter.join();
}

void ConcurrentContract() {
    Fixture fixture;
    constexpr int count = 400;
    std::vector<Signal*> signals;
    for (int i = 0; i < count; ++i) {
        auto* memory = Blizzard::Memory::Allocate(sizeof(Signal));
        signals.push_back(new (memory) Signal);
    }
    std::atomic<bool> start{false};
    auto wait = [&] { while (!start.load()) std::this_thread::yield(); };
    std::thread attach([&] {
        wait();
        for (auto* signal : signals) {
            fixture.Push(1);
            QueueCompletion(&signal->completion, 1);
        }
    });
    std::thread detach([&] {
        wait();
        for (auto* signal : signals) RemoveCompletion(&signal->completion, 1);
    });
    std::thread cancel([&] {
        wait();
        for (int i = 0; i < count; ++i) CancelQueue(1);
    });
    std::thread pop([&] {
        wait();
        FILE_THREAD_CMD command{};
        int queue;
        for (int i = 0; i < count; ++i) SelectQueue(&command, 0, &queue);
    });
    start = true;
    attach.join(); detach.join(); cancel.join(); pop.join();
    CancelQueue(1);
    for (auto* signal : signals) {
        if (signal->completion.m_done) {
            Check(signal->Signaled() && signal->completion.m_result > 0);
            Check(signal->completion.m_success);
        } else {
            Check(!signal->Signaled());
            Check(signal->completion.m_result == 123 || signal->completion.m_result == 0);
        }
        signal->~Signal();
        Blizzard::Memory::Free(signal);
    }
}
}

int main() {
    CompletionContract();
    DetachContract();
    SelectionContract();
    WaiterContract();
    ConcurrentContract();
    std::puts("StreamingQueues: completion, detach, cancel, FIFO, selection, reentry and concurrency passed");
}
