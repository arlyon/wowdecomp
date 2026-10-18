#include <atomic>
#include <cerrno>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

namespace {
using Blizzard::Thread::TLSSlot;
using Blizzard::Thread::RegisterLocalStorage;
TLSSlot shared{}, reentry{}, late{}, lateParent{}, exhaustion[129]{};
std::atomic<unsigned> created{}, deleted{}, reentered{}, failures{}, lateDeleted{};
struct Value { unsigned identity; };
void Check(bool success, const char* message) {
    if (!success) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void* New(void* param) { ++created; return new Value{*static_cast<unsigned*>(param)}; }
void Delete(void* pointer) { ++deleted; delete static_cast<Value*>(pointer); }
void* ReenterNew(void* param) { return new unsigned(*static_cast<unsigned*>(param)); }
void ReenterDelete(void* pointer) {
    unsigned remaining = *static_cast<unsigned*>(pointer);
    delete static_cast<unsigned*>(pointer);
    ++reentered;
    if (remaining) {
        --remaining;
        Check(RegisterLocalStorage(&reentry, ReenterNew, &remaining, ReenterDelete) != nullptr,
              "destructor can reconstruct its own TLS value");
    }
}
void LateDelete(void* pointer) { ++lateDeleted; delete static_cast<unsigned*>(pointer); }
void ParentDelete(void* pointer) {
    delete static_cast<unsigned*>(pointer);
    unsigned value = 123;
    Check(RegisterLocalStorage(&late, ReenterNew, &value, LateDelete) != nullptr,
          "destructor can allocate a new slot without deadlocking");
}
void* Borrow(void* pointer) { return pointer; }
}

int main() {
    // Exercise actual OS key exhaustion before the SDK registry fills up.
    // Only this process's newly allocated keys are released afterwards.
    std::vector<pthread_key_t> nativeKeys;
    int allocationError = 0;
    while (nativeKeys.size() < 65536) {
        pthread_key_t key;
        allocationError = pthread_key_create(&key, nullptr);
        if (allocationError) break;
        nativeKeys.push_back(key);
    }
    Check(allocationError == EAGAIN, "pthread key exhaustion reports a real resource failure");
    unsigned initial = 7;
    Check(!RegisterLocalStorage(&shared, New, &initial, Delete) && errno == EAGAIN &&
          !shared.m_key && !System_Thread::s_slotListUsed && !created,
          "failed key allocation publishes neither a slot nor a constructed value");
    for (pthread_key_t key : nativeKeys) Check(!pthread_key_delete(key), "release only fixture-owned native keys");
    constexpr unsigned count = 16;
    std::atomic<unsigned> arrived{};
    std::vector<std::thread> threads;
    for (unsigned identity = 0; identity < count; ++identity) {
        threads.emplace_back([&, identity]() mutable {
            ++arrived;
            while (arrived != count) std::this_thread::yield();
            auto* value = static_cast<Value*>(RegisterLocalStorage(&shared, New, &identity, Delete));
            Check(value && value->identity == identity, "shared slot has isolated per-thread data");
            for (unsigned i = 0; i < 100; ++i)
                Check(RegisterLocalStorage(&shared, New, &identity, Delete) == value, "repeated access retains thread-local identity");
        });
    }
    for (auto& thread : threads) thread.join();
    Check(created == count && deleted == count, "foreign pthread exits destroy each constructed value exactly once");
    Check(System_Thread::s_slotListUsed == 1, "concurrent first use allocates one registry slot");
    unsigned identity = 99;
    Check(RegisterLocalStorage(&shared, New, &identity, Delete) != nullptr, "main-thread construction");
    System_Thread::RunTLSDestructors();
    System_Thread::RunTLSDestructors();
    Check(deleted == count + 1, "repeated manual cleanup does not double destroy");
    Check(RegisterLocalStorage(&shared, New, &identity, Delete) != nullptr, "slot can be reused for a later pool task");
    System_Thread::RunTLSDestructors();
    Check(created == count + 2 && deleted == count + 2, "pooled-task cleanup reconstructs then destroys a new value");
    unsigned remaining = 2;
    Check(RegisterLocalStorage(&reentry, ReenterNew, &remaining, ReenterDelete) != nullptr, "manual reentry fixture");
    System_Thread::RunTLSDestructors();
    Check(reentered == 3, "manual destructor cleanup handles bounded reconstruction");
    std::thread foreign([&] {
        Check(RegisterLocalStorage(&reentry, ReenterNew, &remaining, ReenterDelete) != nullptr, "foreign reentry fixture");
    });
    foreign.join();
    Check(reentered == 6, "pthread destructor cleanup handles the same reconstruction");
    Check(RegisterLocalStorage(&lateParent, ReenterNew, &identity, ParentDelete) != nullptr, "late-slot fixture");
    System_Thread::RunTLSDestructors();
    Check(lateDeleted == 1, "manual cleanup revisits slots registered by a destructor");
    TLSSlot invalid{};
    System_Thread::InternalSetLocalStorage(&invalid, nullptr);
    Check(errno == EINVAL, "unallocated slot assignment is rejected");
    Check(!RegisterLocalStorage(nullptr, New, &identity, Delete) && errno == EINVAL, "null slot is rejected");
    Check(!RegisterLocalStorage(&shared, nullptr, &identity, Delete) && errno == EINVAL, "null constructor is rejected");
    const unsigned available = 128 - System_Thread::s_slotListUsed;
    for (unsigned i = 0; i < available; ++i)
        Check(RegisterLocalStorage(&exhaustion[i], Borrow, &identity, nullptr) == &identity, "allocate within registry capacity");
    Check(!RegisterLocalStorage(&exhaustion[available], Borrow, &identity, nullptr) && errno == ENOSPC &&
          !exhaustion[available].m_key && System_Thread::s_slotListUsed == 128,
          "slot exhaustion fails without publishing an invalid key or overrunning the registry");
    System_Thread::ShutdownThreadLocalStorage();
    if (!failures) std::puts("PASS: native pthread TLS isolation, publication, destructor reentry, task cleanup and exhaustion");
    return failures ? 1 : 0;
}
