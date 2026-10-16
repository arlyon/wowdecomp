#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <sched.h>
#include <thread>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

namespace {
using Record = Blizzard::Thread::ThreadRecord;
std::atomic<unsigned> failures{}, destroyed{};
Blizzard::Thread::TLSSlot taskLocal{};
void Check(bool success, const char* message) {
    if (!success) { ++failures; std::fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno); }
}
void* NewValue(void*) { return new unsigned(123); }
void DeleteValue(void* value) { delete static_cast<unsigned*>(value); ++destroyed; }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
};
uint32_t Block(void* param) {
    auto& gate = *static_cast<Gate*>(param);
    Check(Blizzard::Thread::RegisterLocalStorage(&taskLocal, NewValue, nullptr, DeleteValue) != nullptr, "managed thread constructs real TLS");
    Record* self = System_Thread::GetThreadRecord();
    Check(self && !Blizzard::Thread::WaitForever(self) && errno == EDEADLK, "self-wait fails instead of deadlocking");
    System_Thread::ShutdownThreadSystem();
    Check(errno == EPERM, "worker cannot shut down or free its own thread system");
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] { return gate.released; });
    return 0xC0FFEE;
}
void Enter(Gate& gate) {
    std::unique_lock<std::mutex> lock(gate.mutex);
    Check(gate.changed.wait_for(lock, std::chrono::seconds(3), [&] { return gate.entered; }), "callback enters within deadline");
}
void Release(Gate& gate) {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
    gate.changed.notify_all();
}
bool Destroyed(unsigned target) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (destroyed < target && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return destroyed >= target;
}
size_t RegistrySize() {
    Blizzard::Lock::ScopedMutex lock(System_Thread::s_registryMutex);
    return System_Thread::s_ThreadRegistry ? System_Thread::s_ThreadRegistry->size() : 0;
}
uint32_t Task(void* param) {
    auto* value = static_cast<unsigned*>(Blizzard::Thread::RegisterLocalStorage(&taskLocal, NewValue, nullptr, DeleteValue));
    Check(value && *value == 123, "each pool task receives freshly constructed TLS");
    if (value) *value = 999;
    return *static_cast<unsigned*>(param);
}
}

int main() {
    System_Thread::InitThreadSystem();
    Check(System_Thread::GetThreadRecord() == System_Thread::s_mainThread && RegistrySize() == 1, "main thread bootstrap and registry ownership");
    Record* record = nullptr;
    Gate gate;
    Check(!Blizzard::Thread::Create(nullptr, nullptr, &record, "invalid", 0, 0) && !record && errno == EINVAL, "invalid callback rejected");
    Check(!Blizzard::Thread::Create(Block, &gate, &record, "suspended", 1, 0) && !record && errno == ENOTSUP, "unsupported suspended start is explicit");
    Check(!Blizzard::Thread::Create(Block, &gate, &record, "bad stack", 0, 1) && !record && errno == EINVAL && RegistrySize() == 1,
          "native stack-size launch failure unwinds record and registry");
    const char* name = "SDK managed thread with full diagnostic name";
    Check(Blizzard::Thread::Create(Block, &gate, &record, name, 0, 0) && record, "create managed pthread");
    if (!record) return 2;
    Enter(gate);
    Check(Blizzard::Thread::IsRunning(record) && !std::strcmp(record->m_name, name) && record->m_threadId,
          "live state, kernel identity and untruncated SDK name are published before Create returns");
    int32_t priority = -1;
    Check(System_Thread::GetPriorityLevel(record, &priority) && priority == 3 && System_Thread::SetPriorityLevel(record, 3), "normal priority round-trip uses the actual native priority");
    Check(!System_Thread::SetPriorityLevel(record, 7) && errno == EINVAL, "invalid priority rejected");
    if (System_Thread::SetPriorityLevel(record, 6)) {
        Check(System_Thread::GetPriorityLevel(record, &priority) && priority == 6, "accepted priority boost is observable in the kernel");
        Check(System_Thread::SetPriorityLevel(record, 3), "restore baseline after an authorized priority boost");
    } else Check(errno == EPERM || errno == EACCES, "unprivileged priority boost reports the real permission failure");
    cpu_set_t original{}, restored{};
    Check(sched_getaffinity(record->m_threadId, sizeof(original), &original) == 0, "independent kernel affinity snapshot");
    errno = 0;
    const int32_t affinity = System_Thread::GetWorkerAffinity(record);
    Check(!errno, "read actual worker affinity without truncating high CPUs");
    for (unsigned cpu = 0; cpu < 32; ++cpu) if (CPU_ISSET(cpu, &original)) {
        System_Thread::SetWorkerAffinity(record, static_cast<int32_t>(uint32_t{1} << cpu));
        Check(!errno, "temporarily restrict the owned test thread to an allowed CPU");
        break;
    }
    System_Thread::SetWorkerAffinity(record, affinity);
    Check(!errno && sched_getaffinity(record->m_threadId, sizeof(restored), &restored) == 0 &&
          CPU_EQUAL(&original, &restored), "restore exact kernel affinity, including unrepresentable high CPUs");
    std::atomic<unsigned> waited{};
    std::vector<std::thread> waiters;
    for (unsigned i = 0; i < 8; ++i) waiters.emplace_back([&] { if (Blizzard::Thread::WaitForever(record)) ++waited; });
    Release(gate);
    for (auto& waiter : waiters) waiter.join();
    Check(waited == 8 && !Blizzard::Thread::IsRunning(record) && record->m_result == 0xC0FFEE && destroyed == 1,
          "concurrent waits observe callback result and completed TLS destruction");
    Check(!System_Thread::GetPriorityLevel(record, &priority) && errno == ESRCH,
          "completed thread priority query rejects a stale kernel identity");
    Check(!System_Thread::SetPriorityLevel(record, 3) && errno == ESRCH,
          "completed thread priority update cannot target a recycled kernel identity");
    errno = 0;
    Check(!System_Thread::GetWorkerAffinity(record) && errno == ESRCH,
          "completed affinity query never dereferences a joined pthread handle");
    errno = 0;
    System_Thread::SetWorkerAffinity(record, affinity);
    Check(errno == ESRCH, "completed affinity update cannot accidentally target the calling thread");
    Check(Blizzard::Thread::WaitForever(record) && Blizzard::Thread::Close(record), "repeated wait and final owner close");
    Check(RegistrySize() == 1, "joined record is removed exactly once");
    Gate detached;
    Check(Blizzard::Thread::CreateDetached(Block, &detached, "detached"), "shared detached wrapper launches native thread");
    Enter(detached);
    Release(detached);
    Check(Destroyed(2), "early caller close retains execution ownership until TLS cleanup");
    {
        Blizzard::Thread::ThreadPoolRecord pool(1, 1, 0);
        for (unsigned i = 0; i < 32; ++i) {
            Check(Blizzard::Thread::CreateFromPool(&pool, Task, &i, &record, 0, 0) && record, "reuse shared pool worker");
            if (!record) return 2;
            Check(Blizzard::Thread::WaitForever(record) && record->m_result == i, "pooled wait publishes result and worker availability");
            Check(Blizzard::Thread::Close(record), "pooled caller releases ownership");
        }
    }
    Check(Destroyed(34), "shared pool destroys each task's TLS without double destruction");
    std::vector<std::thread> submitters;
    for (unsigned i = 0; i < 8; ++i) submitters.emplace_back([i]() mutable {
        Record* task = nullptr;
        Check(Blizzard::Thread::CreateFromPool(nullptr, Task, &i, &task, 0, 0), "concurrent default-pool creation");
        if (task) {
            Check(Blizzard::Thread::WaitForever(task) && task->m_result == i, "concurrent default-pool task completes");
            Blizzard::Thread::Close(task);
        }
    });
    for (auto& submitter : submitters) submitter.join();
    unsigned last = 77;
    for (unsigned i = 0; i < 32; ++i)
        Check(Blizzard::Thread::CreateDetached(Task, &last, "exit race"), "detached exits can race the shutdown registry snapshot");
    Record* retained = nullptr;
    Check(Blizzard::Thread::Create(Task, &last, &retained, "retained caller", 0, 0), "caller can retain a handle across orderly shutdown");
    auto idlePool = std::make_unique<Blizzard::Thread::ThreadPoolRecord>(1, 1, 0);
    auto usedPool = std::make_unique<Blizzard::Thread::ThreadPoolRecord>(1, 1, 0);
    Record* retainedTask = nullptr;
    Check(Blizzard::Thread::CreateFromPool(usedPool.get(), Task, &last, &retainedTask, 0, 0) && retainedTask &&
          Blizzard::Thread::WaitForever(retainedTask), "retain a completed task while its explicit pool stays alive");
    System_Thread::ShutdownThreadSystem();
    Check(!System_Thread::s_mainThread && !System_Thread::s_defaultPool && retained &&
          !Blizzard::Thread::IsRunning(retained) && retained->m_result == last,
          "shutdown joins workers while preserving live caller ownership");
    idlePool.reset();
    usedPool.reset();
    if (retainedTask) {
        Check(Blizzard::Thread::WaitForever(retainedTask) && retainedTask->m_result == last,
              "completed task remains usable after shutdown and later explicit-pool destruction");
        Check(Blizzard::Thread::Close(retainedTask), "release retained task without touching the destroyed pool");
    }
    if (retained) Check(Blizzard::Thread::Close(retained), "retained handle can be closed after system shutdown");
    Check(!RegistrySize(), "registry empties after the last legitimate owner closes");
    System_Thread::ShutdownThreadSystem();
    Check(!Blizzard::Thread::Create(Task, nullptr, &record, "after shutdown", 0, 0) && !record && errno == ESHUTDOWN,
          "shutdown is idempotent and new creation fails explicitly");
    if (!failures) std::puts("PASS: native pthread lifecycle, ownership, waits, TLS, shared pools, scheduling and shutdown");
    return failures ? 1 : 0;
}
