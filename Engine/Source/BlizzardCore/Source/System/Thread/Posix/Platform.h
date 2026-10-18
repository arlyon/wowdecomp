#ifndef BLIZZARD_THREAD_POSIX_PLATFORM_H
#define BLIZZARD_THREAD_POSIX_PLATFORM_H

#include <cstdint>
#include <pthread.h>

// OS scheduling state is opaque to the shared pthread lifecycle. Darwin can
// supply these leaves without reproducing registry, publication or join logic.
namespace System_Thread::Platform {
struct Scheduling;
Scheduling* CreateScheduling();
void DestroyScheduling(Scheduling* state);
uint32_t CurrentId();
void SetCurrentName(const char* name);
void CaptureScheduling(Scheduling* state, pthread_t thread, uint32_t id);
bool SetPriority(Scheduling* state, uint32_t id, int32_t level);
bool GetPriority(Scheduling* state, uint32_t id, int32_t* level);
int32_t GetAffinity(Scheduling* state, pthread_t thread);
void SetAffinity(Scheduling* state, pthread_t thread, int32_t mask);
}

#endif
