#include "Engine/Source/BlizzardCore/Source/System/Thread/Posix/Platform.h"
#include <algorithm>
#include <cerrno>
#include <new>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace System_Thread::Platform {
struct Scheduling {
    int baselineNice = 0, priorityError = 0;
    cpu_set_t affinity{};
    bool haveAffinity = false;
};
namespace {
bool Failure(int error) { errno = error; return false; }
int NiceFor(const Scheduling& state, int level) {
    static constexpr int offsets[] = {19, 10, 5, 0, -5, -10, -20};
    return std::clamp(state.baselineNice + offsets[level], -20, 19);
}
}
Scheduling* CreateScheduling() {
    auto* state = new (std::nothrow) Scheduling;
    if (!state) errno = ENOMEM;
    return state;
}
void DestroyScheduling(Scheduling* state) { delete state; }
uint32_t CurrentId() { return static_cast<uint32_t>(syscall(SYS_gettid)); }
void SetCurrentName(const char* name) { pthread_setname_np(pthread_self(), name); }
void CaptureScheduling(Scheduling* state, pthread_t thread, uint32_t id) {
    errno = 0;
    state->baselineNice = getpriority(PRIO_PROCESS, id);
    state->priorityError = errno;
    state->haveAffinity = pthread_getaffinity_np(thread, sizeof(state->affinity), &state->affinity) == 0;
}
bool SetPriority(Scheduling* state, uint32_t id, int32_t level) {
    if (state->priorityError) return Failure(state->priorityError);
    return setpriority(PRIO_PROCESS, id, NiceFor(*state, level)) == 0 || Failure(errno);
}
bool GetPriority(Scheduling* state, uint32_t id, int32_t* level) {
    if (state->priorityError) return Failure(state->priorityError);
    errno = 0;
    const int actual = getpriority(PRIO_PROCESS, id);
    if (errno) return false;
    // Prefer normal when clamping makes several classes equivalent.
    if (actual == NiceFor(*state, 3)) { *level = 3; return true; }
    for (int candidate = 0; candidate <= 6; ++candidate)
        if (actual == NiceFor(*state, candidate)) { *level = candidate; return true; }
    return Failure(ENOTSUP);
}
int32_t GetAffinity(Scheduling* state, pthread_t thread) {
    const int error = pthread_getaffinity_np(thread, sizeof(state->affinity), &state->affinity);
    if (error) { errno = error; return 0; }
    state->haveAffinity = true;
    uint32_t mask = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &state->affinity)) {
        if (cpu >= 32) return 0; // Restore the full snapshot, never a truncated mask.
        mask |= uint32_t{1} << cpu;
    }
    return static_cast<int32_t>(mask);
}
void SetAffinity(Scheduling* state, pthread_t thread, int32_t mask) {
    cpu_set_t requested{};
    if (!mask) {
        if (!state->haveAffinity) { errno = ENODATA; return; }
        requested = state->affinity;
    } else {
        CPU_ZERO(&requested);
        for (unsigned cpu = 0; cpu < 32; ++cpu)
            if (static_cast<uint32_t>(mask) & (uint32_t{1} << cpu)) CPU_SET(cpu, &requested);
    }
    const int error = pthread_setaffinity_np(thread, sizeof(requested), &requested);
    if (error) errno = error;
}
}
