// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <csignal>
#include <cstring>
#include "Engine/Source/BlizzardCore/Source/System/Debug/Native.h"

#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// STATIC: 0x00B325BB
static bool s_continueAfterAssert;

// OFFSET: 0x00427010
void Blizzard::Debug::Breakpoint() {

    if (System_Debug::Native::DebuggerPresent()) {
        std::raise(SIGTRAP);
    }

}

// OFFSET: 0x00439880
void Blizzard::Debug::DefaultCallback(const char* expression, const char* message, const char* fileName, uint32_t lineNumber) {
    char text[2048];

    char canonical[256];
    char directory[512];
    Blizzard::Time::TimeRec rec;

    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);
    Blizzard::String::Format(text, sizeof(text), "%04d/%02d/%02d %02d:%02d:%02d  %s(%d) : Assertion failed: %s  \"%s\"\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, CanonicalStr(fileName, canonical, sizeof(canonical)), lineNumber, expression, message);
    fprintf(stderr, "%s", text);

    // Never fall back to a relative or truncated log destination.
    if (Blizzard::Process::GetProcessDirectory(directory, sizeof(directory)) &&
        std::strlen(directory) + sizeof("Errors/Assert.log") <= sizeof(directory)) {

    Blizzard::String::Append(directory, "Errors", sizeof(directory));

    System_Debug::Native::CreateDirectory(directory);

    Blizzard::String::Append(directory, "/Assert.log", sizeof(directory));

    FILE* stream = nullptr;

    stream = System_Debug::Native::OpenAppend(directory);

    if (stream) {
        fprintf(stream, "%s", text);
        fclose(stream);
    }

    }

    Breakpoint();

    if (!s_continueAfterAssert) {
        abort();
    }
}

// OFFSET: 0x004321B0
void System_Debug::ShutdownProc() {

    // No dbghelp module is initialized; every native diagnostic FILE is scoped
    // to its operation. Repeated shutdown therefore has no retained resources.

}
