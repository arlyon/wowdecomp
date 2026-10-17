#include <chrono>
#include <cstdint>
#include <thread>
#include "Engine/Source/Os/TimeManager.h"

namespace {
int64_t MonotonicClocks() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

OsTimeManager& TimeManager() {
    static OsTimeManager manager(OsTimeManager::BestAvailable);
    return manager;
}
}

OsTimeManager::TimingMethod OsTimeManager::Calibrate() {
    this->m_performanceFrequency = 1000000000;
    this->m_timingTestError = 0;
    return SystemMethod2;
}

OsTimeManager::OsTimeManager(TimingMethod method) {
    const TimingMethod calibrated = this->Calibrate();
    this->m_timingMethod = method == SystemMethod1 ? SystemMethod1 : calibrated;
    this->m_scaleToMs = this->m_timingMethod == SystemMethod1 ? 1.0 : 0.000001;
    this->m_timeBegin = 0.0;
}

uint64_t OsTimeManager::Snapshot() {
    return static_cast<uint64_t>(MonotonicClocks() / 1000000);
}

uint32_t OsTimeGetTestError() {
    return TimeManager().m_timingTestError;
}

OsTimeManager::TimingMethod OsTimeGetTimingMethod() {
    return TimeManager().m_timingMethod;
}

const char* GetTimingMethodName(OsTimeManager::TimingMethod method) {
    switch (method) {
        case OsTimeManager::NotSet: return "[Not Set]";
        case OsTimeManager::BestAvailable: return "[Best Available]";
        case OsTimeManager::SystemMethod1: return "steady_clock (milliseconds)";
        case OsTimeManager::SystemMethod2: return "steady_clock (nanoseconds)";
        default: return "[Unknown]";
    }
}

uint32_t OsGetAsyncTimeMs() {
    return static_cast<uint32_t>(TimeManager().Snapshot());
}

uint64_t OsGetAsyncTimeClocks() {
    const int64_t clocks = MonotonicClocks();
    return static_cast<uint64_t>(TimeManager().m_timingMethod == OsTimeManager::SystemMethod1
        ? clocks / 1000000 : clocks);
}

int64_t OsGetAsyncClocksPerSecond() {
    return TimeManager().m_timingMethod == OsTimeManager::SystemMethod1 ? 1000 : 1000000000;
}

void OsTimeManagerCreate(OsTimeManager::TimingMethod method) {
    // Startup/shutdown, like the Windows implementation, must not race active users.
    TimeManager() = OsTimeManager(method);
}

void OsTimeManagerDestroy() {
    TimeManager() = OsTimeManager(OsTimeManager::BestAvailable);
}

void OsSleep(uint32_t milliseconds) {
    if (milliseconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    } else {
        std::this_thread::yield();
    }
}
