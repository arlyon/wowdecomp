#include "Engine/Source/Storm/Source/SThread.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
constexpr uint32_t failed = 0xFFFFFFFF;
using namespace std::chrono_literals;

// Fail a real pthread launch at its stable OS boundary, not the callback/provider.
static std::atomic<int> failLaunch{0};
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
    void* (*start)(void*), void* argument) {
    int remaining = failLaunch.load();
    if (remaining && failLaunch.fetch_sub(1) == 1) return EAGAIN;
    return __real_pthread_create(thread, attributes, start, argument);
}

static void lazyCriticalSection() {
    CInitCritSect critical;
    std::atomic<int> initialized{0};
    int protectedValue = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 12; ++i) workers.emplace_back([&] {
        for (int j = 0; j < 200; ++j) {
            if (critical.Enter()) { CHECK(protectedValue == 0); ++initialized; }
            CHECK(!critical.Enter()); // Real CCritSect recursion.
            ++protectedValue;
            critical.Leave(); critical.Leave();
        }
    });
    for (auto& worker : workers) worker.join();
    CHECK(initialized == 1 && protectedValue == 2400);
}
struct CallbackData {
    SEvent entered{1, 0};
    SEvent release{1, 0};
    SEvent tlsDone{1, 0};
    std::atomic<uint32_t> id{0};
    std::atomic<int> calls{0};
    bool explicitExit = false;
    size_t stackSize = 0;
    SMutex abandoned;
};
struct ExitNotification {
    SEvent* event = nullptr;
    ~ExitNotification() { if (event) { std::this_thread::sleep_for(2ms); CHECK(event->Set()); } }
};
static thread_local ExitNotification exitNotification;
static uint32_t STORMAPI callback(void* parameter) {
    auto& data = *static_cast<CallbackData*>(parameter);
    ++data.calls;
    data.id = SGetCurrentThreadId();
    CHECK(data.id != SGetCurrentProcessId());
    char name[16];
    CHECK(SGetThreadName(data.id, name, sizeof(name)));
    CHECK(std::strcmp(name, "copied-thread-n") == 0);
    CHECK(SGetCurrentThreadPriority() == 0 && SSetCurrentThreadPriority(0));
    CHECK(!SSetCurrentThreadPriority(1) && errno == ENOTSUP);
    CHECK(SGetCurrentThreadPriority() == 0);
    CHECK(data.abandoned.Wait(0) == 0);
    exitNotification.event = &data.tlsDone;
    pthread_attr_t attributes;
    CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
    void* base;
    CHECK(pthread_attr_getstack(&attributes, &base, &data.stackSize) == 0);
    CHECK(pthread_attr_destroy(&attributes) == 0);
    CHECK(data.entered.Set());
    CHECK(data.release.Wait(5000) == 0);
    if (data.explicitExit) pthread_exit(nullptr);
    return 0x12345678;
}
static void start(SThread& thread, CallbackData& data, uint32_t stackSize = 0) {
    char name[] = "copied-thread-name-too-long";
    CHECK(SThread::Create(callback, &data, thread, name, stackSize));
    std::memset(name, 'x', sizeof(name) - 1);
    CHECK(data.entered.Wait(2000) == 0);
    CHECK(thread.Valid() && thread.Wait(0) == 258);
    CHECK(thread.SetPriority(2));
    CHECK(!thread.SetPriority(0) && errno == ENOTSUP);
    CHECK(!thread.SetPriority(-1) && errno == EINVAL);
    CHECK(!thread.SetPriority(6) && errno == EINVAL);
    char nameCopy[32];
    CHECK(SGetThreadName(data.id, nameCopy, sizeof(nameCopy)) && !std::strcmp(nameCopy, "copied-thread-n"));
    char tiny[1] = {'x'};
    CHECK(SGetThreadName(data.id, tiny, 1) && tiny[0] == 0);
    CHECK(!SGetThreadName(data.id, nullptr, 1) && !SGetThreadName(data.id, tiny, 0));
}
static void joined(SThread& thread, CallbackData& data) {
    CHECK(thread.Wait(2000) == 0 && thread.Wait(0) == 0);
    CHECK(data.tlsDone.Wait(0) == 0); // Completion includes TLS destruction.
    CHECK(data.abandoned.Wait(0) == 0x80 && data.abandoned.Release());
    char name[16];
    CHECK(!SGetThreadName(data.id, name, sizeof(name)) && name[0] == 0);
    CHECK(!thread.SetPriority(2) && errno == ESRCH);
    CHECK(data.calls == 1);
}
static void lifecycleAndWaits() {
    CallbackData a, b;
    b.explicitExit = true;
    SThread first, second;
    start(first, a, 1024 * 1024); start(second, b);
    CHECK(a.stackSize >= 1024 * 1024);
    SSyncObject* threads[] = {&first, &second};
    CHECK(SSyncObject::WaitMultiplePtr(2, threads, 1, 5) == 258);
    a.release.Set();
    CHECK(SSyncObject::WaitMultiplePtr(2, threads, 0, 2000) == 0);
    CHECK(SSyncObject::WaitMultiplePtr(2, threads, 1, 5) == 258);
    b.release.Set();
    CHECK(SSyncObject::WaitMultiplePtr(2, threads, 1, 2000) == 0);
    joined(first, a); joined(second, b);
    first.Close();
    CHECK(!first.Valid() && first.Wait(0) == failed);
    // Reuse a closed handle after all prior calls finish.
    CallbackData again;
    start(first, again); again.release.Set(); joined(first, again);
}
static void earlyClose() {
    CallbackData data;
    auto handle = std::make_unique<SThread>();
    start(*handle, data);
    std::atomic<bool> waiting{false};
    std::thread waiter([&] { waiting = true; CHECK(handle->Wait(2000) == failed); });
    while (!waiting) std::this_thread::yield();
    std::this_thread::sleep_for(2ms);
    handle->Close();
    waiter.join();
    CHECK(handle->Wait(0) == failed && data.calls == 1);
    handle.reset(); // Callback must remain alive, with no access to this object.
    CHECK(data.tlsDone.Wait(0) == 258);
    data.release.Set();
    CHECK(data.tlsDone.Wait(2000) == 0);
    // Quiesce the detached reaper via registry removal before callback storage dies.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    char name[16];
    while (SGetThreadName(data.id, name, sizeof(name))) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(data.abandoned.Wait(0) == 0x80 && data.abandoned.Release());
}
static uint32_t STORMAPI countCallback(void* parameter) {
    ++*static_cast<std::atomic<int>*>(parameter);
    return 1;
}
static void launchFailures() {
    std::atomic<int> calls{0};
    SThread thread;
    CHECK(!SThread::Create(nullptr, &calls, thread, nullptr, 0) && errno == EINVAL);
    CHECK(!SThread::Create(countCallback, &calls, thread, nullptr, 1) && errno == EINVAL);
    for (int ordinal : {1, 2}) {
        failLaunch = ordinal;
        CHECK(!SThread::Create(countCallback, &calls, thread, nullptr, 0) && errno == EAGAIN);
        CHECK(!thread.Valid() && calls == 0);
        failLaunch = 0;
    }
    CHECK(SThread::Create(countCallback, &calls, thread, nullptr, 0));
    CHECK(thread.Wait(2000) == 0 && calls == 1);
    char name[16];
    CHECK(SGetThreadName(SGetCurrentProcessId(), name, sizeof(name)) && !std::strcmp(name, "main"));
    CHECK(!SSetCurrentProcessAffinityMask(1) && errno == ENOTSUP);
}
static uint32_t STORMAPI gated(void* parameter) {
    CHECK(static_cast<SEvent*>(parameter)->Wait(10000) == 0);
    return 0;
}
static void registryCapacity() {
    SEvent release(1, 0);
    std::vector<std::unique_ptr<SThread>> threads;
    for (int i = 1; i < S_Thread::s_maxthreads; ++i) {
        auto thread = std::make_unique<SThread>();
        CHECK(SThread::Create(gated, &release, *thread, nullptr, 1024 * 1024));
        threads.push_back(std::move(thread));
    }
    SThread extra;
    CHECK(!SThread::Create(gated, &release, extra, nullptr, 0) && errno == EAGAIN);
    CHECK(!extra.Valid());
    release.Set();
    for (auto& thread : threads) CHECK(thread->Wait(3000) == 0);
    S_Thread::s_threadCrit.Enter();
    CHECK(S_Thread::s_numthreads == 1);
    S_Thread::s_threadCrit.Leave();
}
int main() {
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(50ms);
        if (!done) std::abort();
    });
    lazyCriticalSection(); launchFailures(); lifecycleAndWaits(); earlyClose(); registryCapacity();
    done = true; watchdog.join();
    std::puts("StormThreadTest passed");
}
