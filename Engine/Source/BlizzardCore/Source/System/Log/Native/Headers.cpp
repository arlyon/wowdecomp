// Native-extension algorithm; the original Windows translation unit is unchanged.
#include "Engine/Source/Platform/Platform.h"

#line 2

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../Native.h"

#line 9
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// OFFSET: 0x004383E0
char* System_Log::DefaultPreamble() {
    Blizzard::Time::TimeRec rec;
    Blizzard::Time::BreakTime(s_startTime, rec);

    char computerName[256];
    if (NativeHostname(computerName, sizeof(computerName))) {
        Blizzard::String::Copy(computerName, "<unknown>", sizeof(computerName));
    }
    computerName[sizeof(computerName) - 1] = '\0';

#line 32

    char* preamble = static_cast<char*>(malloc(4096));
    Blizzard::String::Format(preamble, 4096, "\n#-----------------------------------------------------------\n# System started at %04d-%02d-%02d %02d:%02d:%02d.%04d\n# system: %s\n#-----------------------------------------------------------\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, rec.nanosecond / 100000, computerName);

    return preamble;
}

// OFFSET: 0x004384B0
char* System_Log::DefaultDayHeader() {
    Blizzard::Time::TimeRec rec;
    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);

    char computerName[256];
    if (NativeHostname(computerName, sizeof(computerName))) {
        Blizzard::String::Copy(computerName, "<unknown>", sizeof(computerName));
    }
    computerName[sizeof(computerName) - 1] = '\0';

#line 53

    char* header = static_cast<char*>(malloc(4096));
    Blizzard::String::Format(header, 4096, "\n#-----------------------------------------------------------\n# %04d-%02d-%02d\n# system: %s\n#-----------------------------------------------------------\n", rec.year, rec.month, rec.day, computerName);

    return header;
}

// OFFSET: 0x004388D0
uint32_t System_Log::LogThread(void* param) {

    return NativeLogThread(param);

#line 81
}
