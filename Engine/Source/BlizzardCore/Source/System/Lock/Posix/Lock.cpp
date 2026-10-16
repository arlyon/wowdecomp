#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

namespace Blizzard { namespace Lock {
struct NativeEvent {
    std::mutex mutex;
    std::condition_variable changed;
    bool signaled = false;
};

namespace {
std::recursive_mutex& NativeMutex(Mutex& mutex) {
    return *std::launder(reinterpret_cast<std::recursive_mutex*>(mutex.m_storage));
}
}

int32_t Atomic::Increment() { return __atomic_add_fetch(&m_value, 1, __ATOMIC_SEQ_CST); }
int32_t Atomic::Decrement() { return __atomic_sub_fetch(&m_value, 1, __ATOMIC_SEQ_CST); }

void MutexCreate(Mutex& mutex) { new (mutex.m_storage) std::recursive_mutex(); }
void MutexDestroy(Mutex& mutex) { NativeMutex(mutex).~recursive_mutex(); }
void MutexEnter(Mutex& mutex) { NativeMutex(mutex).lock(); }
void MutexLeave(Mutex& mutex) { NativeMutex(mutex).unlock(); }
RMutex::RMutex() { MutexCreate(m_mutex); }
RMutex::~RMutex() { MutexDestroy(m_mutex); }
ScopedMutex::ScopedMutex(Mutex& mutex) : m_mutex(&mutex) { MutexEnter(mutex); }
ScopedMutex::~ScopedMutex() { MutexLeave(*m_mutex); }

void EventCreate(Event& event) { event = new NativeEvent; }
void EventDestroy(Event& event) { delete event; event = nullptr; }
void EventSet(Event& event) {
    if (!event) { errno = EINVAL; return; }
    {
        std::lock_guard<std::mutex> lock(event->mutex);
        event->signaled = true;
    }
    event->changed.notify_one();
}

Event* EventWait(uint32_t timeout, Event& event) {
    if (!event) { errno = EINVAL; return nullptr; }
    std::unique_lock<std::mutex> lock(event->mutex);
    if (timeout == UINT32_MAX) {
        event->changed.wait(lock, [&] { return event->signaled; });
    } else if (!event->changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return event->signaled; })) {
        return nullptr;
    }
    // The original CreateEventA uses manualReset=FALSE: one waiter consumes
    // a signal, and repeated sets while signaled must coalesce, not count.
    event->signaled = false;
    return &event;
}

void EventWaitForever(Event& event) { EventWait(UINT32_MAX, event); }
EventObject::EventObject() { EventCreate(m_event); }
EventObject::~EventObject() { EventDestroy(m_event); }
}}
