#include "Engine/Source/Storm/Source/SLock.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
constexpr uint32_t timeout = 258;
constexpr uint32_t failed = 0xFFFFFFFF;
using namespace std::chrono_literals;

static void events() {
    SEvent manual(1, 1), automatic(0, 1);
    CHECK(manual.Wait(0) == 0 && manual.Wait(0) == 0);
    CHECK(manual.Reset() && manual.Wait(1) == timeout);
    CHECK(automatic.Wait(0) == 0 && automatic.Wait(0) == timeout);
    CHECK(automatic.Set() && automatic.Set());
    CHECK(automatic.Wait(0) == 0 && automatic.Wait(0) == timeout);
    CHECK(automatic.Set() && automatic.Reset() && automatic.Wait(0) == timeout);
    std::thread signal([&] { std::this_thread::sleep_for(10ms); manual.Set(); });
    CHECK(manual.Wait(failed) == 0);
    signal.join();
    for (int mode : {0, 1}) {
        SEvent event(mode, 0);
        std::atomic<int> ready{0}, successes{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
            ++ready;
            auto result = event.Wait(150);
            CHECK(result == 0 || result == timeout);
            if (result == 0) ++successes;
        });
        while (ready != 4) std::this_thread::yield();
        event.Set();
        for (auto& thread : threads) thread.join();
        CHECK(successes == (mode ? 4 : 1));
    }
}
static void mutexes() {
    SMutex mutex;
    CHECK(mutex.Wait(0) == 0 && mutex.Wait(0) == 0);
    std::thread contender([&] { CHECK(!mutex.Release()); CHECK(mutex.Wait(10) == timeout); });
    contender.join();
    CHECK(mutex.Release());
    std::thread stillOwned([&] { CHECK(mutex.Wait(10) == timeout); });
    stillOwned.join();
    CHECK(mutex.Release() && !mutex.Release());
    std::thread next([&] { CHECK(mutex.Wait(1000) == 0); CHECK(mutex.Release()); });
    next.join();
    mutex.Create(1, nullptr);
    CHECK(mutex.Release());
    mutex.Create(0, "unsupported");
    CHECK(!mutex.Valid() && errno == ENOTSUP);
    SMutex a, b;
    std::thread abandonA([&] { CHECK(a.Wait(0) == 0 && a.Wait(0) == 0); });
    std::thread abandonB([&] { CHECK(b.Wait(0) == 0); });
    abandonA.join(); abandonB.join();
    SSyncObject* objects[] = {&a, &b};
    CHECK(SSyncObject::WaitMultiplePtr(2, objects, 1, 0) == 0x80);
    CHECK(a.Release() && b.Release());
    std::thread abandonAgain([&] { CHECK(a.Wait(0) == 0); });
    abandonAgain.join();
    CHECK(a.Wait(0) == 0x80 && a.Release());
    CHECK(a.Wait(0) == 0 && a.Release());
}
static void multiwait() {
    SSemaphore invalid(-1, 2), zero(0, 0), sem(1, 2);
    CHECK(!invalid.Valid() && !zero.Valid());
    CHECK(!sem.Signal(0) && !sem.Signal(2) && !sem.Signal(failed));
    CHECK(sem.Wait(0) == 0 && sem.Wait(0) == timeout);
    CHECK(sem.Signal(2) && !sem.Signal(1));
    CHECK(sem.Wait(0) == 0 && sem.Wait(0) == 0 && sem.Wait(0) == timeout);
    SEvent first(0, 1), second(0, 0);
    SSyncObject* pair[] = {&first, &second};
    CHECK(SSyncObject::WaitMultiplePtr(2, pair, 1, 10) == timeout);
    CHECK(first.Wait(0) == 0); // Timeout did not consume a subset.
    first.Set(); second.Set();
    CHECK(SSyncObject::WaitMultiplePtr(2, pair, 0, 0) == 0);
    CHECK(SSyncObject::WaitMultiplePtr(2, pair, 0, 0) == 1);
    first.Set(); second.Set();
    CHECK(SSyncObject::WaitMultiplePtr(2, pair, 1, 0) == 0);
    CHECK(first.Wait(0) == timeout && second.Wait(0) == timeout);
    SSyncObject* filtered[] = {nullptr, &invalid, &first, &second};
    second.Set();
    CHECK(SSyncObject::WaitMultiplePtr(4, filtered, 0, 0) == 1);
    CHECK(SSyncObject::WaitMultiplePtr(0, pair, 0, 0) == failed);
    CHECK(SSyncObject::WaitMultiplePtr(65, pair, 0, 0) == failed);
    CHECK(SSyncObject::WaitMultiplePtr(1, nullptr, 0, 0) == failed);
    SSyncObject* duplicates[] = {&first, &first};
    CHECK(SSyncObject::WaitMultiplePtr(2, duplicates, 1, 0) == failed);
    SSyncObject* empty[] = {nullptr, &invalid};
    CHECK(SSyncObject::WaitMultiplePtr(2, empty, 0, 0) == failed);
    std::vector<SEvent*> allocated;
    SSyncObject* maximum[64];
    for (auto& object : maximum) { allocated.push_back(new SEvent(1, 1)); object = allocated.back(); }
    CHECK(SSyncObject::WaitMultiplePtr(64, maximum, 1, 0) == 0);
    for (auto* object : allocated) delete object;
    SMutex unconsumed;
    SSemaphore token(1, 1);
    SEvent blocked(0, 0);
    SSyncObject* mixed[] = {&unconsumed, &token, &blocked};
    CHECK(SSyncObject::WaitMultiplePtr(3, mixed, 1, 5) == timeout);
    std::thread ownsAfterTimeout([&] { CHECK(unconsumed.Wait(0) == 0); CHECK(unconsumed.Release()); });
    ownsAfterTimeout.join();
    CHECK(token.Wait(0) == 0);
    std::thread signal([&] { std::this_thread::sleep_for(10ms); first.Set(); second.Set(); });
    CHECK(SSyncObject::WaitMultiplePtr(2, pair, 1, failed - 1) == 0);
    signal.join();
    // Two waitAll contenders must acquire both semaphore tokens or neither.
    SSemaphore left(1, 1), right(0, 1);
    SSyncObject* tokens[] = {&left, &right};
    std::atomic<int> successes{0};
    auto contender = [&] { auto result = SSyncObject::WaitMultiplePtr(2, tokens, 1, 100); if (!result) ++successes; else CHECK(result == timeout); };
    std::thread one(contender), two(contender);
    right.Signal(1);
    one.join(); two.join();
    CHECK(successes == 1 && left.Wait(0) == timeout && right.Wait(0) == timeout);
}
static void closeAndLifetime() {
    for (int i = 0; i < 25; ++i) {
        SEvent event(0, 0);
        std::atomic<bool> ready{false};
        std::thread waiter([&] { ready = true; CHECK(event.Wait(1000) == failed); });
        while (!ready) std::this_thread::yield();
        if (i % 2) std::this_thread::sleep_for(2ms);
        event.Close(); // Also legal before the waiter enters: no live handle => failed.
        waiter.join();
        CHECK(!event.Valid() && !event.Set() && !event.Reset());
        event.Close();
    }
    // An expired weak registry entry must not retain destroyed mutex resources.
    std::thread owner([] { for (int i = 0; i < 100; ++i) { SMutex mutex; CHECK(mutex.Wait(0) == 0); } });
    owner.join();
}
static void rwLocks() {
    SCritSect critical;
    critical.Enter(); critical.Enter(); critical.Leave(); critical.Leave();
    CSRWLock lock;
    lock.Enter(0);
    SEvent sharedEntered(1, 0);
    std::thread shared([&] { lock.Enter(0); sharedEntered.Set(); lock.Leave(0); });
    CHECK(sharedEntered.Wait(1000) == 0); // A second reader really enters concurrently.
    shared.join();
    std::atomic<bool> attempting{false}, acquired{false};
    std::thread writer([&] { attempting = true; lock.Enter(1); acquired = true; lock.Leave(1); });
    while (!attempting) std::this_thread::yield();
    std::this_thread::sleep_for(10ms);
    lock.Enter(0); // Reader preference: nested read cannot deadlock behind queued writer.
    CHECK(!acquired);
    lock.Leave(0); lock.Leave(0);
    writer.join(); CHECK(acquired);
    std::atomic<int> readers{0}, writers{0};
    int value = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) threads.emplace_back([&, i] {
        for (int j = 0; j < 300; ++j) {
            bool write = i < 2;
            lock.Enter(write);
            if (write) { CHECK(writers.fetch_add(1) == 0); CHECK(readers == 0); ++value; --writers; }
            else { ++readers; CHECK(writers == 0); CHECK(value >= 0); --readers; }
            lock.Leave(write);
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(value == 600);
}
int main() {
    // Bound every test, including bugs in infinite-wait and lock paths.
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(50ms);
        if (!done) std::abort();
    });
    events(); mutexes(); multiwait(); closeAndLifetime(); rwLocks();
    done = true; watchdog.join();
    std::puts("StormSyncTest passed");
}
