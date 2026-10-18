#include <cerrno>
#include <cstdio>
#include <mutex>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_set>
#include "Engine/Source/Os/OsCall.h"

namespace {
struct CallState {
    std::mutex mutex;
    std::unordered_set<OsCallContext*> contexts;
};
CallState& State() {
    static CallState state;
    return state;
}

// All crosslinks are accessed under State().mutex; TLS owns the thread object.
void Detach(OsCallThread* thread) {
    if (thread->m_context) {
        thread->m_context->m_thread = nullptr;
        thread->m_context = nullptr;
    }
}
void Release(OsCallThread*& thread) {
    std::lock_guard<std::mutex> lock(State().mutex);
    if (!thread) return;
    Detach(thread);
    delete thread;
    thread = nullptr;
}
struct Registration {
    OsCallThread* thread = nullptr;
    ~Registration() { Release(thread); }
};
Registration& Current() {
    // Construct shared state before TLS, so main-thread TLS cleanup precedes
    // shared-state destruction. Other registered threads must be joined.
    (void)State();
    thread_local Registration registration;
    return registration;
}
bool IsLive(OsCallContext* context) {
    if (State().contexts.count(context)) return true;
    errno = EINVAL;
    return false;
}
}

OsCallContext::~OsCallContext() = default;

void OsCallInitialize(const char* name) {
    Registration& current = Current();
    auto* thread = new OsCallThread{};
    thread->m_threadId = static_cast<uint32_t>(syscall(SYS_gettid));
    // There is no owned Windows HANDLE equivalent: TLS owns this registration.
    std::snprintf(thread->m_name, sizeof(thread->m_name), "%s [%u]",
        name ? name : "", thread->m_threadId);
    std::lock_guard<std::mutex> lock(State().mutex);
    if (current.thread) {
        Detach(current.thread);
        delete current.thread;
    }
    current.thread = thread;
}

void OsCallDestroy() {
    Release(Current().thread);
}

void* OsCallInitializeContext(const char* name) {
    auto* context = new OsCallContext{};
    context->unused_000C = -1;
    std::snprintf(context->m_name, sizeof(context->m_name), "%s", name ? name : "");
    try {
        std::lock_guard<std::mutex> lock(State().mutex);
        State().contexts.insert(context);
    } catch (...) {
        delete context;
        throw;
    }
    return context;
}

void OsCallSetContext(void* handle) {
    if (!handle) return;
    Registration& current = Current();
    std::lock_guard<std::mutex> lock(State().mutex);
    auto* context = static_cast<OsCallContext*>(handle);
    if (!IsLive(context) || !current.thread) return;
    Detach(current.thread);
    if (context->m_thread) Detach(context->m_thread);
    context->m_thread = current.thread;
    current.thread->m_context = context;
}

void OsCallResetContext(void* handle) {
    if (!handle) return;
    Registration& current = Current();
    std::lock_guard<std::mutex> lock(State().mutex);
    auto* context = static_cast<OsCallContext*>(handle);
    if (!IsLive(context) || !current.thread) return;
    // A context may have transferred since this thread last used it.
    if (context->m_thread == current.thread) Detach(current.thread);
}

void OsCallDestroyContext(void* handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(State().mutex);
    auto* context = static_cast<OsCallContext*>(handle);
    if (!IsLive(context)) return;
    if (context->m_thread) Detach(context->m_thread);
    State().contexts.erase(context);
    delete context;
}
