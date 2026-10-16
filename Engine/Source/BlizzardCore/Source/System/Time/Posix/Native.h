#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_TIME_POSIX_NATIVE_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_TIME_POSIX_NATIVE_H

#include "Engine/Source/BlizzardCore/Source/System/Time/Types.h"

namespace System_Time {
void NativeBreakTime(Blizzard::Time::Timestamp time, Blizzard::Time::TimeRec& rec);
Blizzard::Time::Timestamp NativeMakeTime(const Blizzard::Time::TimeRec& rec);
Blizzard::Time::Timestamp NativeTimestamp();
int64_t NativeMilliseconds();
}

#endif
