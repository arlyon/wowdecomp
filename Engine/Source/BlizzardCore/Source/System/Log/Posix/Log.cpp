#include <cerrno>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Log/Native.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// Header templates and the worker entry point are compiled separately from
// System/Log/Native; this provider owns only the POSIX lifecycle leaves.

namespace {
struct LogLifecycle {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entering = 0;
    bool stopped = false;
    bool shutdownComplete = false;
};
LogLifecycle& Lifecycle() {
    // Blizzard::Using invokes NativeShutdown from cross-TU static destructors.
    // Ordinary globals here can already be destroyed at that point on Darwin.
    // Retain only the admission primitives for process lifetime; NativeShutdown
    // still joins the worker, flushes logs and releases all logger resources.
    static auto* state = new LogLifecycle;
    return *state;
}
}

int System_Log::NativeHostname(char* name, uint32_t count) {
    return gethostname(name, count);
}

bool System_Log::NativeIsAbsolutePath(const char* path) {
    return path && path[0] == '/';
}
bool System_Log::NativeCreateDirectory(const char* path, bool recursive) {
    if (!path || !*path) return true;
    std::error_code error;
    if (recursive) std::filesystem::create_directories(path, error);
    else std::filesystem::create_directory(path, error);
    if (error) { errno = error.value(); return false; }
    return true;
}
int System_Log::NativeOpen(FILE** stream, const char* path, const char* mode) {
    *stream = std::fopen(path, mode);
    return *stream ? 0 : errno;
}

bool System_Log::NativeAcquire(int32_t logClass) {
    auto& [lifecycle, changed, entering, stopped, shutdownComplete] = Lifecycle();
    if (logClass < 0) return false;
    {
        std::unique_lock<std::mutex> lock(lifecycle);
        if (stopped) return false;
        if (!s_state) {
            s_startTime = Blizzard::Time::GetTimestamp();
            Blizzard::Lock::MutexCreate(s_mutex);
            if (!Blizzard::Thread::Create(LogThread, nullptr, &s_thread, "", 0, 0x10000)) {
                Blizzard::Lock::MutexDestroy(s_mutex);
                s_thread = nullptr;
                return false;
            }
            s_state = 1;
            s_threadRunning = true;
            s_threadDone = false;
        }
        ++entering;
    }
    // Admission must not hold lifecycle while waiting for the recursive logger
    // mutex: a writer may itself call Acquire before releasing its outer lock.
    Blizzard::Lock::MutexEnter(s_mutex);
    {
        std::lock_guard<std::mutex> lock(lifecycle);
        --entering;
        changed.notify_all();
    }
    Init(logClass);
    return true; // The existing caller owns the matching MutexLeave.
}

void System_Log::NativeShutdown() {
    auto& [lifecycle, changed, entering, stopped, shutdownComplete] = Lifecycle();
    {
        std::unique_lock<std::mutex> lock(lifecycle);
        if (stopped) {
            changed.wait(lock, [] { return Lifecycle().shutdownComplete; });
            return;
        }
        stopped = true;
        s_state = -1;
        changed.wait(lock, [] { return Lifecycle().entering == 0; });
    }
    if (s_thread) {
        Blizzard::Thread::WaitForever(s_thread);
        Blizzard::Thread::Close(s_thread);
        s_thread = nullptr;
        // No new writers can be admitted, and all admitted waiters now own or
        // have released s_mutex. Taking it drains those existing writers.
        Blizzard::Lock::MutexEnter(s_mutex);
        FlushLogs(true);
        // Classes 1..3 inherit the default directory pointer; do not free aliases.
        free(s_Prefs ? s_Prefs[0].m_directory : nullptr);
        free(s_Prefs);
        free(s_stringPool);
        s_Prefs = nullptr;
        s_stringPool = nullptr;
        s_prefCount = 0;
        Blizzard::Lock::MutexLeave(s_mutex);
        Blizzard::Lock::MutexDestroy(s_mutex);
    }
    {
        std::lock_guard<std::mutex> lock(lifecycle);
        s_threadRunning = false;
        shutdownComplete = true;
        changed.notify_all();
    }
}

uint32_t System_Log::NativeLogThread(void*) {
    auto& [lifecycle, changed, entering, stopped, shutdownComplete] = Lifecycle();
    uint32_t lastFlush = static_cast<uint32_t>(Blizzard::Time::Milliseconds());
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(lifecycle);
            if (stopped) break;
        }
        Blizzard::Thread::Sleep(100);
        uint32_t now = static_cast<uint32_t>(Blizzard::Time::Milliseconds());
        if (now - lastFlush > 5000) {
            lastFlush = now;
            if (Acquire(0)) {
                FlushLogs(false);
                Blizzard::Lock::MutexLeave(s_mutex);
            }
        }
    }
    std::lock_guard<std::mutex> lock(lifecycle);
    s_threadDone = true;
    return 0;
}
