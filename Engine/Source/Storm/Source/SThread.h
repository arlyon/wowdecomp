#ifndef ENGINE_SOURCE_STORM_SOURCE_STHREAD_H
#define ENGINE_SOURCE_STORM_SOURCE_STHREAD_H

#include <cstddef>
#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#else
#include <memory>
#include <mutex>
#include <pthread.h>
struct SNativeThreadControl;
#endif
#include "Engine/Source/Storm/H/SCritSect.h"
#include "Engine/Source/Storm/Source/SLock.h"

class SThread : public SSyncObject {
    public:
    static int32_t Create(uint32_t (STORMAPI* threadProc)(void*), void* param, SThread& thread, char* threadName, uint32_t stackSize);

    int32_t SetPriority(int32_t priority);
#if !defined(_WIN32)
    // Create/reuse and destruction require quiescence of calls on this handle.
    // Close never cancels execution; completion means actual pthread/TLS exit.
    private:
    std::shared_ptr<SNativeThreadControl> m_control;
#endif
};

class CInitCritSect {
    public:
#if defined(_WIN32)
    /* 0000 */ LONG m_spinLock;
    /* 0004 */ CCritSect* m_critsect;
    /* 0008 */ uint8_t m_critsectData[sizeof(CCritSect)];
#else
    std::once_flag m_once;
    CCritSect* m_critsect = nullptr;
    alignas(CCritSect) uint8_t m_critsectData[sizeof(CCritSect)];
    CInitCritSect() = default;
    // Owners and callers must be quiescent before destruction.
    ~CInitCritSect();
#endif

    int32_t Enter();
    void Leave();
};

namespace S_Thread {
    struct SThreadTrack {
        /* 0000 */ int32_t suspended;
        /* 0004 */ int32_t live;
        /* 0008 */ uint32_t threadId;
#if defined(_WIN32)
        /* 000C */ HANDLE threadH;
#else
        pthread_t threadH;
        SNativeThreadControl* control;
#endif
        /* 0010 */ char name[16];
    };

    const int32_t s_maxthreads = 128;

#if defined(_WIN32)
    extern SThreadTrack s_threads[s_maxthreads];
    extern int32_t s_numthreads;
    extern CInitCritSect s_threadCrit;
#else
    // Registry storage outlives detached reapers and TLS teardown.
    extern SThreadTrack (&s_threads)[s_maxthreads];
    extern int32_t& s_numthreads;
    extern CInitCritSect& s_threadCrit;
#endif

    uint32_t STORMAPI s_SLaunchThread(void* threadParam);
}

#endif
