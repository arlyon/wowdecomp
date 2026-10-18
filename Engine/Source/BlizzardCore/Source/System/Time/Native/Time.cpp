// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cmath>
#include <cstdint>

#include "Engine/Source/BlizzardCore/Source/System/Time/Native.h"

#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// STATIC: 0x009E56C0
const int32_t System_Time::s_daysBeforeMonth[13] = { 0, 31, 59, 90, 120, 151, 182, 212, 243, 273, 304, 334, 365 };

// STATIC: 0x00AB9350
uint64_t System_Time::timeScaleCycles = 0xFFFFFFFFFFFFFFFF;

// STATIC: 0x00B325B9
bool System_Time::s_usePerformanceCounter;

// STATIC: 0x00B33264
Blizzard::Lock::Atomic System_Time::s_tickLock;

// STATIC: 0x00B33270
int64_t System_Time::s_absBegin;

// STATIC: 0x00B33278
uint32_t System_Time::s_lastTickCount;

// STATIC: 0x00B33280
double System_Time::timeScaleMilliseconds;

// STATIC: 0x00B33288
Blizzard::Time::Timestamp System_Time::s_gmBegin;

// STATIC: 0x00B33290
uint64_t System_Time::s_tickCountHigh;

// STATIC: 0x00B33298
double System_Time::timeScaleNanoseconds;

// STATIC: 0x00B332A0
double System_Time::timeScaleSeconds;

// STATIC: 0x00B332A8
double System_Time::timeScaleMicroseconds;

// OFFSET: 0x00428F00
Blizzard::Time::Timestamp Blizzard::Time::FromFileTime(uint64_t fileTime) {
    if (fileTime <= 0x0077A5D6AA8BF851) {
        return static_cast<Timestamp>(0x8000000000000000);
    }

    if (fileTime <= 0x030701FFA04E87AE) {
        return static_cast<Timestamp>(fileTime * 100 + 0x5143382561530000);
    }

    return 0x7FFFFFFFFFFFFFFF;
}

// OFFSET: 0x00429070
void Blizzard::Time::BreakTime(Timestamp time, TimeRec& rec) {
    int32_t nanosecond = static_cast<int32_t>(time % 1000000000);

    if (nanosecond < 0) {
        nanosecond += 1000000000;
    }

    System_Time::NativeBreakTime(time, rec);
    rec.nanosecond = nanosecond;

    bool leap = !(rec.year % 400) || (rec.year % 100 && !(rec.year % 4));

    rec.yearday = System_Time::s_daysBeforeMonth[rec.month - 1] + rec.day - 1;

    if (leap && rec.month > 2) {
        rec.yearday++;
    }
}

// OFFSET: 0x00429170
Blizzard::Time::Timestamp Blizzard::Time::MakeTime(const TimeRec& rec) {

    return System_Time::NativeMakeTime(rec);

}

// OFFSET: 0x00435D80
Blizzard::Time::Timestamp Blizzard::Time::GetTimestamp() {

    return System_Time::NativeTimestamp();

}

// OFFSET: 0x00435E20
int64_t Blizzard::Time::Milliseconds() {

    return System_Time::NativeMilliseconds();

}
