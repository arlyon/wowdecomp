#include "Engine/Source/Storm/Source/SThread.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <new>
#include <sys/syscall.h>
#include <unistd.h>

struct SNativeThreadControl {
    std::mutex mutex;
    std::condition_variable changed;
    SEvent completion{1, 0};
    uint32_t (STORMAPI* callback)(void*);
    void* argument;
    pthread_t worker{};
    uint32_t id = 0;
    bool identified = false;
    bool released = false;
    bool abort = false;
    bool running = false;
    char name[16]{};
};

namespace {
struct Registry {
    S_Thread::SThreadTrack threads[S_Thread::s_maxthreads]{};
    int32_t count = 0;
    CInitCritSect critical;
};
Registry& registry() {
    // Detached reapers may finish during static destruction.
    static Registry* value = new Registry;
    return *value;
}
struct RegistryGuard {
    RegistryGuard() { S_Thread::s_threadCrit.Enter(); }
    ~RegistryGuard() { S_Thread::s_threadCrit.Leave(); }
};
void copyName(char* destination, const char* source, size_t capacity) {
    size_t size = std::min(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, size);
    destination[size] = 0;
}
void stopped(void* parameter) {
    auto& control = *static_cast<SNativeThreadControl*>(parameter);
    std::lock_guard<std::mutex> guard(control.mutex);
    // Stop priority operations before pthread_join can invalidate pthread_t.
    control.running = false;
}
void* workerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<SNativeThreadControl>> holder(
        static_cast<std::shared_ptr<SNativeThreadControl>*>(parameter));
    auto control = *holder;
    holder.reset();
    {
        std::unique_lock<std::mutex> guard(control->mutex);
        control->id = SGetCurrentThreadId();
        control->identified = true;
        control->changed.notify_all();
        control->changed.wait(guard, [&] { return control->released; });
        if (control->abort) return nullptr;
        control->running = true;
    }
    pthread_setname_np(pthread_self(), control->name);
    pthread_cleanup_push(stopped, control.get());
    S_Thread::s_SLaunchThread(control.get());
    pthread_cleanup_pop(1);
    return nullptr;
}
void* reapMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<SNativeThreadControl>> holder(
        static_cast<std::shared_ptr<SNativeThreadControl>*>(parameter));
    auto control = *holder;
    holder.reset();
    // Includes pthread cleanup handlers and every TLS destructor, not just callback return.
    int error = pthread_join(control->worker, nullptr);
    if (error) std::terminate(); // This private joinable worker has exactly one joiner.
    {
        RegistryGuard guard;
        for (int32_t i = 0; i < S_Thread::s_numthreads; ++i) {
            if (S_Thread::s_threads[i].control != control.get()) continue;
            for (int32_t j = i + 1; j < S_Thread::s_numthreads; ++j)
                S_Thread::s_threads[j - 1] = S_Thread::s_threads[j];
            S_Thread::s_threads[--S_Thread::s_numthreads] = {};
            break;
        }
    }
    control->completion.Set();
    return nullptr;
}
int32_t setNormalPriority(pthread_t thread, int32_t priority) {
    if (priority != 0) { errno = ENOTSUP; return 0; }
    sched_param parameters{};
    int error = pthread_setschedparam(thread, SCHED_OTHER, &parameters);
    if (error) { errno = error; return 0; }
    return 1;
}
}

namespace S_Thread {
SThreadTrack (&s_threads)[s_maxthreads] = registry().threads;
int32_t& s_numthreads = registry().count;
CInitCritSect& s_threadCrit = registry().critical;
uint32_t STORMAPI s_SLaunchThread(void* parameter) {
    auto& control = *static_cast<SNativeThreadControl*>(parameter);
    return control.callback(control.argument);
}
}

int32_t CInitCritSect::Enter() {
    int32_t initialized = 0;
    std::call_once(m_once, [&] {
        m_critsect = new (m_critsectData) CCritSect;
        // The initialization winner must hold the lock before other entrants can return.
        m_critsect->Enter();
        initialized = 1;
    });
    if (!initialized) m_critsect->Enter();
    return initialized;
}
void CInitCritSect::Leave() { m_critsect->Leave(); }
CInitCritSect::~CInitCritSect() { if (m_critsect) m_critsect->~CCritSect(); }

int32_t SThread::Create(uint32_t (STORMAPI* threadProc)(void*), void* parameter,
    SThread& thread, char* threadName, uint32_t stackSize) {
    if (!threadProc) { errno = EINVAL; return 0; }
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) { errno = error; return 0; }
    if (stackSize) error = pthread_attr_setstacksize(&attributes, stackSize);
    if (error) { pthread_attr_destroy(&attributes); errno = error; return 0; }
    std::shared_ptr<SNativeThreadControl> control;
    std::unique_ptr<std::shared_ptr<SNativeThreadControl>> launch, reap;
    try {
        control = std::make_shared<SNativeThreadControl>();
        control->callback = threadProc;
        control->argument = parameter;
        copyName(control->name, threadName ? threadName : "", sizeof(control->name));
        launch = std::make_unique<std::shared_ptr<SNativeThreadControl>>(control);
        reap = std::make_unique<std::shared_ptr<SNativeThreadControl>>(control);
    } catch (const std::bad_alloc&) {
        pthread_attr_destroy(&attributes);
        errno = ENOMEM;
        return 0;
    }
    RegistryGuard registryGuard;
    if (!S_Thread::s_numthreads) {
        auto& main = S_Thread::s_threads[S_Thread::s_numthreads++];
        main.threadId = static_cast<uint32_t>(getpid());
        main.live = 1;
        copyName(main.name, "main", sizeof(main.name));
    }
    if (S_Thread::s_numthreads == S_Thread::s_maxthreads) {
        pthread_attr_destroy(&attributes);
        errno = EAGAIN;
        return 0;
    }
    error = pthread_create(&control->worker, &attributes, workerMain, launch.get());
    pthread_attr_destroy(&attributes);
    if (error) { errno = error; return 0; }
    launch.release();
    std::unique_lock<std::mutex> guard(control->mutex);
    control->changed.wait(guard, [&] { return control->identified; });
    // Reaper needs only a small/default stack, independent of requested callback stack.
    error = pthread_attr_init(&attributes);
    if (!error) {
        error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (!error) {
            pthread_t reaper;
            error = pthread_create(&reaper, &attributes, reapMain, reap.get());
        }
        pthread_attr_destroy(&attributes);
    }
    if (error) {
        control->abort = true;
        control->released = true;
        control->changed.notify_all();
        guard.unlock();
        if (pthread_join(control->worker, nullptr)) std::terminate();
        errno = error;
        return 0;
    }
    reap.release();
    auto& track = S_Thread::s_threads[S_Thread::s_numthreads++];
    track = {};
    track.threadId = control->id;
    track.threadH = control->worker;
    track.control = control.get();
    track.live = 1;
    copyName(track.name, control->name, sizeof(track.name));
    thread.Close();
    thread.m_control = control;
    // Both objects are private to this quiescent Create; later Set/Close use SLock's coordinator.
    thread.m_opaqueData = control->completion.m_opaqueData;
    control->running = true;
    control->released = true;
    control->changed.notify_all();
    return 1;
}
int32_t SThread::SetPriority(int32_t priority) {
    if (priority < 0 || priority >= 6) { errno = EINVAL; return 0; }
    if (!m_control || !Valid()) { errno = ESRCH; return 0; }
    std::lock_guard<std::mutex> guard(m_control->mutex);
    if (!m_control->running) { errno = ESRCH; return 0; }
    // SThread's category 2 is Windows THREAD_PRIORITY_NORMAL (raw priority 0).
    if (priority != 2) { errno = ENOTSUP; return 0; }
    return setNormalPriority(m_control->worker, 0);
}
uint32_t STORMAPI SGetCurrentThreadId() { return static_cast<uint32_t>(syscall(SYS_gettid)); }
uint32_t STORMAPI SGetCurrentProcessId() { return static_cast<uint32_t>(getpid()); }
int32_t STORMAPI SGetCurrentThreadPriority() {
    int policy;
    sched_param parameters{};
    int error = pthread_getschedparam(pthread_self(), &policy, &parameters);
    if (!error && policy == SCHED_OTHER && parameters.sched_priority == 0) return 0;
    errno = error ? error : ENOTSUP;
    return std::numeric_limits<int32_t>::max();
}
int32_t STORMAPI SSetCurrentThreadPriority(int32_t priority) { return setNormalPriority(pthread_self(), priority); }
int32_t STORMAPI SSetCurrentProcessAffinityMask(size_t) { errno = ENOTSUP; return 0; }
int32_t STORMAPI SGetThreadName(uint32_t id, char* buffer, int32_t capacity) {
    if (!buffer || capacity < 1) return 0;
    *buffer = 0;
    RegistryGuard guard;
    // Prefer the newest live record if the kernel recycled a recently exited TID
    // before its detached reaper removed the older record.
    for (int32_t i = S_Thread::s_numthreads - 1; i >= 0; --i) {
        if (S_Thread::s_threads[i].threadId != id) continue;
        copyName(buffer, S_Thread::s_threads[i].name, std::min<size_t>(capacity, 16));
        return 1;
    }
    return 0;
}
