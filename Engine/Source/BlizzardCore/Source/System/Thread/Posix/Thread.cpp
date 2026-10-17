#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <string>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Posix/Platform.h"
#include <thread>
#include <type_traits>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

namespace System_Thread {
bool s_initialized;
Blizzard::Lock::Mutex s_mutex, s_registryMutex;
Blizzard::Thread::TLSSlot s_threadRecordTLS{}, s_stackTraceEntryPointTLS{};
Blizzard::Thread::ThreadRecord* s_mainThread;
std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>* s_ThreadRegistry;
int32_t s_DefaultStackSize;
}

namespace {
using Record = Blizzard::Thread::ThreadRecord;
struct Control {
    std::mutex mutex;
    std::condition_variable changed;
    pthread_t thread{};
    bool started = false, ready = false, published = false;
    bool complete = false, joining = false, joined = false, main = false;
    System_Thread::Platform::Scheduling* scheduling = nullptr;
    std::string name;
    ~Control() {
        if (started && !joined) pthread_detach(thread);
        System_Thread::Platform::DestroyScheduling(scheduling);
    }
};
std::once_flag initialized;
std::recursive_mutex creationMutex;
std::atomic<bool> stopping{false};
Control& State(Record* record) { return *static_cast<Control*>(record->m_handle); }
bool Failure(int error) { errno = error; return false; }
uint32_t ThreadId() { return System_Thread::Platform::CurrentId(); }

Record* Allocate(uint32_t (*function)(void*), void* param, const char* name) {
    std::unique_ptr<Control> control(new (std::nothrow) Control);
    if (!control) { errno = ENOMEM; return nullptr; }
    control->scheduling = System_Thread::Platform::CreateScheduling();
    if (!control->scheduling) return nullptr;
    try { control->name = name ? name : ""; }
    catch (const std::bad_alloc&) { errno = ENOMEM; return nullptr; }
    auto* record = static_cast<Record*>(std::malloc(sizeof(Record)));
    if (!record) { errno = ENOMEM; return nullptr; }
    new (record) Record{};
    record->m_handle = control.get();
    record->m_function = function;
    record->m_param = param;
    record->m_name = control->name.data();
    record->m_refCount.m_value = 2; // execution and caller ownership
    try { System_Thread::AddToRegistry(record); }
    catch (const std::bad_alloc&) { record->~ThreadRecord(); std::free(record); errno = ENOMEM; return nullptr; }
    control.release();
    return record;
}

void CaptureScheduling(Record& record) {
    Control& control = State(&record);
    System_Thread::Platform::CaptureScheduling(control.scheduling, control.thread, record.m_threadId);
}
void* Start(void* pointer) { System_Thread::ThreadStarter(pointer); return nullptr; }

}

void System_Thread::AddToRegistry(Record* record) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);
    if (!s_ThreadRegistry) s_ThreadRegistry = new std::remove_reference_t<decltype(*s_ThreadRegistry)>;
    s_ThreadRegistry->insert(record);
}
void System_Thread::RemoveFromRegistry(Record* record) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);
    if (!s_ThreadRegistry) return;
    s_ThreadRegistry->erase(record);
    if (s_ThreadRegistry->empty()) { delete s_ThreadRegistry; s_ThreadRegistry = nullptr; }
}
void System_Thread::InitMainThread() {
    s_mainThread = Allocate(nullptr, nullptr, "Main Thread");
    if (!s_mainThread) throw std::bad_alloc();
    auto& control = State(s_mainThread);
    control.thread = pthread_self();
    control.main = true;
    s_mainThread->m_threadId = ThreadId();
    s_mainThread->m_refCount.m_value = 1;
    s_mainThread->m_state = 1;
    CaptureScheduling(*s_mainThread);
    InternalSetLocalStorage(&s_threadRecordTLS, s_mainThread);
}
void System_Thread::InitThreadSystem() {
    std::call_once(initialized, [] {
        static const bool locksCreated = [] {
            Blizzard::Lock::MutexCreate(s_mutex);
            Blizzard::Lock::MutexCreate(s_registryMutex);
            return true;
        }();
        (void)locksCreated;
        if (!AllocateLocalStorage(&s_threadRecordTLS, nullptr) || !AllocateLocalStorage(&s_stackTraceEntryPointTLS, nullptr))
            throw std::bad_alloc();
        InitMainThread();
        s_initialized = true;
    });
}
Record* System_Thread::GetThreadRecord() {
    InitThreadSystem();
    const uint32_t key = __atomic_load_n(&s_threadRecordTLS.m_key, __ATOMIC_ACQUIRE);
    return static_cast<Record*>(pthread_getspecific(static_cast<pthread_key_t>(~key)));
}
void System_Thread::SetStackTraceEntryPoint(void* entryPoint) {
    InitThreadSystem();
    InternalSetLocalStorage(&s_stackTraceEntryPointTLS, entryPoint);
}
void System_Thread::InitStackTraceEntryPoint() { SetStackTraceEntryPoint(__builtin_return_address(0)); }
Record* System_Thread::NewThread(uint32_t (*function)(void*), void* param, const char* name) {
    std::lock_guard<std::recursive_mutex> lock(creationMutex);
    InitThreadSystem();
    if (stopping) { errno = ESHUTDOWN; return nullptr; }
    return Allocate(function, param, name);
}

bool System_Thread::ValidateNativeCreate(uint32_t (*function)(void*), Record** output, int32_t flags, int32_t stackSize) {
    if (!output) return Failure(EINVAL);
    *output = nullptr;
    if (!function || stackSize < 0) return Failure(EINVAL);
    // There is no matching resume operation in this SDK interface.
    if (flags) return Failure(ENOTSUP);
    if (stopping) return Failure(ESHUTDOWN);
    return true;
}

bool Blizzard::Thread::Create(uint32_t (*function)(void*), void* param, Record** output, const char* name, int32_t flags, int32_t stackSize) {
    std::lock_guard<std::recursive_mutex> creation(creationMutex);
    if (!System_Thread::ValidateNativeCreate(function, output, flags, stackSize)) return false;
    Record* record = System_Thread::NewThread(function, param, name);
    if (!record) return false;
    auto& control = State(record);
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (!error) {
        const int32_t requested = stackSize ? stackSize : System_Thread::s_DefaultStackSize;
        if (requested < 0) error = EINVAL;
        else if (requested > 0) error = pthread_attr_setstacksize(&attributes, static_cast<size_t>(requested));
        if (!error) {
            std::unique_lock<std::mutex> lock(control.mutex);
            error = pthread_create(&control.thread, &attributes, Start, record);
            if (!error) {
                control.started = true;
                control.changed.wait(lock, [&] { return control.ready; });
                *output = record;
                record->m_state = 1;
                control.published = true;
                control.changed.notify_all();
            }
        }
        pthread_attr_destroy(&attributes);
    }
    if (!error) return true;
    record->m_refCount.m_value = 1; // no execution owner on launch failure
    Close(record);
    return Failure(error);
}

uint32_t System_Thread::ThreadStarter(void* pointer) {
    auto* record = static_cast<Record*>(pointer);
    auto& control = State(record);
    InternalSetLocalStorage(&s_threadRecordTLS, record);
    InitStackTraceEntryPoint();
    {
        std::unique_lock<std::mutex> lock(control.mutex);
        record->m_threadId = ThreadId();
        CaptureScheduling(*record);
        if (!control.name.empty()) {
            const std::string name = control.name.substr(0, 15);
            System_Thread::Platform::SetCurrentName(name.c_str());
        }
        control.ready = true;
        control.changed.notify_all();
        control.changed.wait(lock, [&] { return control.published; });
    }
    const uint32_t result = record->m_function(record->m_param);
    record->m_result = result;
    RunTLSDestructors();
    CompleteNativeTask(record);
    InternalSetLocalStorage(&s_threadRecordTLS, nullptr);
    Blizzard::Thread::Close(record);
    return result;
}

void System_Thread::CompleteNativeTask(Record* record) {
    auto& control = State(record);
    std::lock_guard<std::mutex> lock(control.mutex);
    record->m_state = -1;
    control.complete = true;
    control.changed.notify_all();
}

bool Blizzard::Thread::Close(Record* record) {
    if (!record) return Failure(EINVAL);
    {
        // Serialize the last release with shutdown's registry snapshot so a
        // zero-reference record cannot be resurrected while being destroyed.
        Blizzard::Lock::ScopedMutex lock(System_Thread::s_registryMutex);
        if (record->m_refCount.Decrement() > 0) return true;
        System_Thread::RemoveFromRegistry(record);
    }
    delete static_cast<Control*>(record->m_handle);
    record->~ThreadRecord();
    std::free(record);
    return true;
}

bool Blizzard::Thread::WaitForever(Record* record) {
    if (!record || !record->m_handle) return Failure(EINVAL);
    auto& control = State(record);
    auto* current = System_Thread::GetThreadRecord();
    if (record == current || (current && current->m_worker && current->m_worker->m_task == record)) return Failure(EDEADLK);
    if (control.main) return Failure(EINVAL);
    record->m_refCount.Increment();
    struct Release { Record* record; ~Release() { Blizzard::Thread::Close(record); } } release{record};
    std::unique_lock<std::mutex> lock(control.mutex);
    control.changed.wait(lock, [&] { return control.complete; });
    if (!control.started) return true; // a pooled task has no separate OS thread
    control.changed.wait(lock, [&] { return !control.joining; });
    if (control.joined) return true;
    control.joining = true;
    lock.unlock();
    const int error = pthread_join(control.thread, nullptr);
    lock.lock();
    control.joined = !error;
    control.joining = false;
    control.changed.notify_all();
    return !error || Failure(error);
}

bool Blizzard::Thread::IsRunning(Record* record) {
    return record ? record->m_state.load() >= 0 : Failure(EINVAL);
}
bool Blizzard::Thread::Sleep(uint32_t milliseconds) {
    if (milliseconds) std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    else std::this_thread::yield();
    return true;
}

bool System_Thread::SetPriorityLevel(Record* record, int32_t level) {
    if (!record || !record->m_threadId || level < 0 || level > 6) return Failure(EINVAL);
    auto& control = State(record);
    std::lock_guard<std::mutex> lock(control.mutex);
    if (control.complete || control.joined) return Failure(ESRCH);
    return Platform::SetPriority(control.scheduling, record->m_threadId, level);
}
bool System_Thread::GetPriorityLevel(Record* record, int32_t* level) {
    if (!record || !record->m_threadId || !level) return Failure(EINVAL);
    auto& control = State(record);
    std::lock_guard<std::mutex> lock(control.mutex);
    if (control.complete || control.joined) return Failure(ESRCH);
    return Platform::GetPriority(control.scheduling, record->m_threadId, level);
}

int32_t System_Thread::GetWorkerAffinity(Record* record) {
    if (!record || (!State(record).started && !State(record).main)) { errno = EINVAL; return 0; }
    auto& control = State(record);
    std::lock_guard<std::mutex> lock(control.mutex);
    if (control.complete || control.joined) { errno = ESRCH; return 0; }
    return Platform::GetAffinity(control.scheduling, control.thread);
}
void System_Thread::SetWorkerAffinity(Record* record, int32_t mask) {
    if (!record || (!State(record).started && !State(record).main)) { errno = EINVAL; return; }
    auto& control = State(record);
    std::lock_guard<std::mutex> lock(control.mutex);
    if (control.complete || control.joined) { errno = ESRCH; return; }
    Platform::SetAffinity(control.scheduling, control.thread, mask);
}

void System_Thread::ShutdownNativeThreads() {
    {
        std::lock_guard<std::recursive_mutex> creation(creationMutex);
        if (stopping) return;
        if (GetThreadRecord() != s_mainThread) { errno = EPERM; return; }
        stopping = true;
    }
    if (s_defaultPool) { delete s_defaultPool; s_defaultPool = nullptr; }
    std::vector<Record*> active;
    {
        Blizzard::Lock::ScopedMutex lock(s_registryMutex);
        if (s_ThreadRegistry) active.reserve(s_ThreadRegistry->size());
        if (s_ThreadRegistry) for (Record* record : *s_ThreadRegistry) if (record != s_mainThread) {
            record->m_refCount.Increment();
            active.push_back(record);
        }
    }
    // Caller-owned pools can outlive system shutdown. Wake all of their OS
    // workers as well, without deleting the pools or consuming ready events
    // needed by their later destructors. Task records have no OS thread.
    for (Record* record : active) if (State(record).started && record->m_worker)
        Blizzard::Lock::EventSet(record->m_worker->m_wakeEvent.m_event);
    for (Record* record : active) { Blizzard::Thread::WaitForever(record); Blizzard::Thread::Close(record); }
    RunTLSDestructors();
    InternalSetLocalStorage(&s_threadRecordTLS, nullptr);
    if (s_mainThread) {
        Record* main = s_mainThread;
        s_mainThread = nullptr;
        Blizzard::Thread::Close(main);
    }
}
