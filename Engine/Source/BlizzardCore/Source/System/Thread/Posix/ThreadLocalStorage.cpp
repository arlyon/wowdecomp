#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <pthread.h>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

namespace {
// Keys and the registry have process lifetime, as in the original backend.
// Do not delete keys while a foreign thread can still use its TLS value.
pthread_mutex_t slotMutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t Key(const Blizzard::Thread::TLSSlot* slot) {
    return __atomic_load_n(&slot->m_key, __ATOMIC_ACQUIRE);
}

bool Allocate(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    if (!slot) { errno = EINVAL; return false; }
    const int locked = pthread_mutex_lock(&slotMutex);
    if (locked) { errno = locked; return false; }
    if (Key(slot)) { pthread_mutex_unlock(&slotMutex); return true; }
    if (System_Thread::s_slotListUsed >= 128) {
        pthread_mutex_unlock(&slotMutex);
        errno = ENOSPC;
        return false;
    }
    pthread_key_t key;
    const int allocated = pthread_key_create(&key, destructor);
    if (allocated) {
        pthread_mutex_unlock(&slotMutex);
        errno = allocated;
        return false;
    }
    // TLSSlot has a 32-bit encoding even when pthread_key_t is wider.
    if (static_cast<uint64_t>(key) >= UINT32_MAX) {
        pthread_key_delete(key);
        pthread_mutex_unlock(&slotMutex);
        errno = EOVERFLOW;
        return false;
    }
    const uint32_t encoded = ~static_cast<uint32_t>(key);
    slot->m_destructor = destructor;
    System_Thread::s_slotList[System_Thread::s_slotListUsed++] = slot;
    __atomic_store_n(&slot->m_key, encoded, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&slotMutex);
    return true;
}
}

bool System_Thread::AllocateLocalStorage(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    return Allocate(slot, destructor);
}

bool System_Thread::AllocateTLSSlot(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    return Allocate(slot, destructor);
}

void System_Thread::InternalSetLocalStorage(const Blizzard::Thread::TLSSlot* slot, const void* value) {
    if (!slot || !Key(slot)) { errno = EINVAL; return; }
    const int result = pthread_setspecific(static_cast<pthread_key_t>(~Key(slot)), const_cast<void*>(value));
    if (result) errno = result;
}

void* Blizzard::Thread::RegisterLocalStorage(TLSSlot* slot, void* (*constructor)(void*), void* param, TLSDestructor destructor) {
    if (!slot || !constructor) { errno = EINVAL; return nullptr; }
    if (!Key(slot) && !Allocate(slot, destructor)) return nullptr;
    const pthread_key_t key = static_cast<pthread_key_t>(~Key(slot));
    if (void* existing = pthread_getspecific(key)) return existing;
    void* value = constructor(param);
    if (!value) return nullptr;
    const int result = pthread_setspecific(key, value);
    if (result) {
        if (destructor) destructor(value);
        errno = result;
        return nullptr;
    }
    return value;
}

void System_Thread::RunTLSDestructors() {
    struct Entry { pthread_key_t key; Blizzard::Thread::TLSDestructor destructor; };
    // Match POSIX's bounded re-entry rule. Clear each value before invoking
    // user code, outside the registry lock, so callbacks may register TLS.
    for (unsigned pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; ++pass) {
        std::array<Entry, 128> entries{};
        const int locked = pthread_mutex_lock(&slotMutex);
        if (locked) { errno = locked; return; }
        const uint32_t count = s_slotListUsed;
        for (uint32_t i = 0; i < count; ++i) {
            const auto* slot = s_slotList[i];
            entries[i] = {static_cast<pthread_key_t>(~Key(slot)), slot->m_destructor};
        }
        pthread_mutex_unlock(&slotMutex);
        bool called = false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = entries[i];
            if (!entry.destructor) continue;
            void* value = pthread_getspecific(entry.key);
            if (!value) continue;
            const int cleared = pthread_setspecific(entry.key, nullptr);
            if (cleared) { errno = cleared; return; }
            called = true;
            entry.destructor(value);
        }
        if (!called) return;
    }
}

void System_Thread::ShutdownThreadLocalStorage() {
    // Managed threads are joined by their owner; foreign threads receive the
    // same callbacks from pthreads on exit. Retain keys for those late exits.
    RunTLSDestructors();
}
