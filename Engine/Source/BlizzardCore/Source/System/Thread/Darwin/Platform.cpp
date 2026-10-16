#include "Engine/Source/BlizzardCore/Source/System/Thread/Posix/Platform.h"
#include <algorithm>
#include <cerrno>
#include <new>
#include <sched.h>

namespace System_Thread::Platform {
struct Scheduling {
    pthread_t thread{};
    int policy = SCHED_OTHER;
    int baseline = 0;
    int error = 0;
};
namespace {
bool Result(int error) { if (error) errno = error; return error == 0; }
int Priority(const Scheduling& state, int level) {
    static constexpr int offsets[] = {-15, -10, -5, 0, 5, 10, 15};
    return std::clamp(state.baseline + offsets[level],
        sched_get_priority_min(state.policy), sched_get_priority_max(state.policy));
}
}
Scheduling* CreateScheduling() { return new (std::nothrow) Scheduling; }
void DestroyScheduling(Scheduling* state) { delete state; }
uint32_t CurrentId() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<uint32_t>(id);
}
void SetCurrentName(const char* name) { pthread_setname_np(name); }
void CaptureScheduling(Scheduling* state, pthread_t thread, uint32_t) {
    state->thread = thread;
    sched_param params{};
    state->error = pthread_getschedparam(thread, &state->policy, &params);
    state->baseline = params.sched_priority;
}
bool SetPriority(Scheduling* state, uint32_t, int32_t level) {
    if (state->error) return Result(state->error);
    if (level < 0 || level > 6) return Result(EINVAL);
    sched_param params{};
    params.sched_priority = Priority(*state, level);
    return Result(pthread_setschedparam(state->thread, state->policy, &params));
}
bool GetPriority(Scheduling* state, uint32_t, int32_t* level) {
    int policy;
    sched_param params{};
    if (!Result(pthread_getschedparam(state->thread, &policy, &params))) return false;
    if (policy != state->policy) return Result(ENOTSUP);
    if (params.sched_priority == Priority(*state, 3)) { *level = 3; return true; }
    for (int i = 0; i < 7; ++i)
        if (params.sched_priority == Priority(*state, i)) { *level = i; return true; }
    return Result(ENOTSUP);
}
int32_t GetAffinity(Scheduling*, pthread_t) { errno = ENOTSUP; return 0; }
void SetAffinity(Scheduling*, pthread_t, int32_t) { errno = ENOTSUP; }
}
