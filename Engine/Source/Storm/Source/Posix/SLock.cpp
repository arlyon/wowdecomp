#include "Engine/Source/Storm/Source/SLock.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <thread>
#include <vector>

struct SNativeSyncState {
    enum Kind { Event, Mutex, Semaphore } kind;
    bool closed = false;
    bool manual = false;
    bool abandoned = false;
    uint32_t count = 0;
    uint32_t maximum = 0;
    std::thread::id owner;
    uint32_t depth = 0;
    explicit SNativeSyncState(Kind type) : kind(type) {}
};

namespace {
constexpr uint32_t failed = 0xFFFFFFFF;
constexpr uint32_t timedOut = 258;
struct Coordinator {
    std::mutex mutex;
    std::condition_variable changed;
};
Coordinator& coordinator() {
    // TLS owner-exit callbacks may run after static object destruction.
    static Coordinator* instance = new Coordinator;
    return *instance;
}
struct OwnedMutexes {
    std::vector<std::weak_ptr<SNativeSyncState>> states;
    ~OwnedMutexes() {
        auto& sync = coordinator();
        std::lock_guard<std::mutex> guard(sync.mutex);
        for (auto& weak : states) {
            auto state = weak.lock();
            if (state && !state->closed && state->owner == std::this_thread::get_id()) {
                state->owner = {};
                state->depth = 0;
                state->abandoned = true;
            }
        }
        sync.changed.notify_all();
    }
    void Track(const std::shared_ptr<SNativeSyncState>& state) {
        for (auto it = states.begin(); it != states.end();) {
            if (auto existing = it->lock()) {
                if (existing == state) return;
                ++it;
            } else {
                it = states.erase(it);
            }
        }
        states.push_back(state);
    }
};
thread_local OwnedMutexes owned;
bool available(const SNativeSyncState& state) {
    if (state.kind == SNativeSyncState::Mutex) {
        return state.depth == 0 || (state.owner == std::this_thread::get_id()
            && state.depth < std::numeric_limits<uint32_t>::max());
    }
    return state.count != 0;
}
bool acquire(const std::shared_ptr<SNativeSyncState>& state) {
    if (state->kind == SNativeSyncState::Mutex) {
        if (!state->depth) {
            owned.Track(state);
            state->owner = std::this_thread::get_id();
        }
        ++state->depth;
        bool abandoned = state->abandoned;
        state->abandoned = false;
        return abandoned;
    }
    if (state->kind == SNativeSyncState::Semaphore || !state->manual) --state->count;
    return false;
}
}

SCritSect::SCritSect() = default;
SCritSect::~SCritSect() = default;
void SCritSect::Enter() { m_critsect.lock(); }
void SCritSect::Leave() { m_critsect.unlock(); }
SSyncObject::SSyncObject() = default;
SSyncObject::~SSyncObject() { Close(); }
void SSyncObject::Close() {
    auto& sync = coordinator();
    std::lock_guard<std::mutex> guard(sync.mutex);
    if (m_opaqueData) {
        m_opaqueData->closed = true;
        m_opaqueData.reset();
        sync.changed.notify_all();
    }
}
int32_t SSyncObject::Valid() {
    std::lock_guard<std::mutex> guard(coordinator().mutex);
    return m_opaqueData != nullptr;
}
uint32_t SSyncObject::Wait(uint32_t timeout) {
    SSyncObject* object = this;
    return WaitMultiplePtr(1, &object, 0, timeout);
}
uint32_t STORMAPI SSyncObject::WaitMultiplePtr(uint32_t count, SSyncObject** objects, int32_t waitAll, uint32_t timeout) {
    if (!count || count > 64 || !objects) return failed;
    auto& sync = coordinator();
    std::unique_lock<std::mutex> guard(sync.mutex);
    std::array<std::shared_ptr<SNativeSyncState>, 64> states;
    uint32_t size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!objects[i] || !objects[i]->m_opaqueData) continue;
        auto state = objects[i]->m_opaqueData;
        // Windows forbids duplicate handles in a multiple-object wait.
        for (uint32_t j = 0; j < size; ++j) if (states[j] == state) return failed;
        states[size++] = state;
    }
    if (!size) return failed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        for (uint32_t i = 0; i < size; ++i) if (states[i]->closed) return failed;
        if (waitAll) {
            bool ready = true;
            for (uint32_t i = 0; i < size; ++i) ready = ready && available(*states[i]);
            if (ready) {
                uint32_t result = 0;
                for (uint32_t i = 0; i < size; ++i) {
                    if (acquire(states[i]) && result == 0) result = 0x80 + i;
                }
                return result;
            }
        } else {
            for (uint32_t i = 0; i < size; ++i) {
                if (available(*states[i])) return acquire(states[i]) ? 0x80 + i : i;
            }
        }
        if (!timeout) return timedOut;
        if (timeout == failed) sync.changed.wait(guard);
        else if (sync.changed.wait_until(guard, deadline) == std::cv_status::timeout) {
            // Recheck availability once at the deadline, without extending it.
            timeout = 0;
        }
    }
}
SEvent::SEvent(int32_t manualReset, int32_t initialState) {
    m_opaqueData = std::make_shared<SNativeSyncState>(SNativeSyncState::Event);
    m_opaqueData->manual = manualReset != 0;
    m_opaqueData->count = initialState != 0;
}
int32_t SEvent::Set() {
    auto& sync = coordinator();
    std::lock_guard<std::mutex> guard(sync.mutex);
    if (!m_opaqueData) return 0;
    m_opaqueData->count = 1;
    sync.changed.notify_all();
    return 1;
}
int32_t SEvent::Reset() {
    std::lock_guard<std::mutex> guard(coordinator().mutex);
    if (!m_opaqueData) return 0;
    m_opaqueData->count = 0;
    return 1;
}
SMutex::SMutex() { Create(0, nullptr); }
void SMutex::Create(int32_t initialOwner, const char* name) {
    auto& sync = coordinator();
    std::lock_guard<std::mutex> guard(sync.mutex);
    if (m_opaqueData) m_opaqueData->closed = true;
    m_opaqueData.reset();
    sync.changed.notify_all();
    if (name) { errno = ENOTSUP; return; }
    m_opaqueData = std::make_shared<SNativeSyncState>(SNativeSyncState::Mutex);
    if (initialOwner) acquire(m_opaqueData);
}
int32_t SMutex::Release() {
    auto& sync = coordinator();
    std::lock_guard<std::mutex> guard(sync.mutex);
    if (!m_opaqueData || !m_opaqueData->depth || m_opaqueData->owner != std::this_thread::get_id()) return 0;
    if (!--m_opaqueData->depth) {
        m_opaqueData->owner = {};
        sync.changed.notify_all();
    }
    return 1;
}
SSemaphore::SSemaphore(int32_t initialCount, int32_t maximumCount) {
    if (initialCount < 0 || maximumCount <= 0 || initialCount > maximumCount) return;
    m_opaqueData = std::make_shared<SNativeSyncState>(SNativeSyncState::Semaphore);
    m_opaqueData->count = initialCount;
    m_opaqueData->maximum = maximumCount;
}
int32_t SSemaphore::Signal(uint32_t count) {
    auto& sync = coordinator();
    std::lock_guard<std::mutex> guard(sync.mutex);
    if (!m_opaqueData || !count || count > m_opaqueData->maximum - m_opaqueData->count) return 0;
    m_opaqueData->count += count;
    sync.changed.notify_all();
    return 1;
}

struct SNativeRWState {
    std::mutex mutex;
    std::condition_variable changed;
    uint32_t readers = 0;
    bool writer = false;
};
void SRWLock::SURWLockInitialize(volatile SURWLOCK* lock) { lock->state = new SNativeRWState; }
void SRWLock::SURWLockDelete(volatile SURWLOCK* lock) { delete lock->state; lock->state = nullptr; }
void SRWLock::SURWLockEnter(volatile SURWLOCK* lock, int32_t forwriting) {
    auto& state = *lock->state;
    std::unique_lock<std::mutex> guard(state.mutex);
    state.changed.wait(guard, [&] { return !state.writer && (!forwriting || !state.readers); });
    if (forwriting) state.writer = true;
    else ++state.readers;
}
void SRWLock::SURWLockLeave(volatile SURWLOCK* lock, int32_t fromwriting) {
    auto& state = *lock->state;
    std::lock_guard<std::mutex> guard(state.mutex);
    if (fromwriting) state.writer = false;
    else --state.readers;
    state.changed.notify_all();
}
CSRWLock::CSRWLock() { SRWLock::SURWLockInitialize(&m_opaqueData); }
CSRWLock::~CSRWLock() { SRWLock::SURWLockDelete(&m_opaqueData); }
void CSRWLock::Enter(int32_t forwriting) { SRWLock::SURWLockEnter(&m_opaqueData, forwriting); }
void CSRWLock::Leave(int32_t fromwriting) { SRWLock::SURWLockLeave(&m_opaqueData, fromwriting); }
