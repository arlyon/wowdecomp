#if defined(WOWDECOMP_ARCH_64) || !defined(_WIN32)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#else
#include "Engine/Source/BlizzardCore/Source/System/Debug/Linux/Native.h"
#endif
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// STATIC: 0x00B325BB
static bool s_continueAfterAssert;

#if defined(_WIN32)
// STATIC: 0x00B332F0
static Blizzard::Lock::Mutex s_dbgHelpLock;

// STATIC: 0x00B382CC
static HMODULE s_dbgHelp;

// STATIC: 0x00B382D0
static bool s_dbgHelpInitialized;

#endif

// OFFSET: 0x00427010
void Blizzard::Debug::Breakpoint() {
#if defined(_WIN32)
    __try {
        __debugbreak();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    if (System_Debug::Native::DebuggerPresent()) {
        std::raise(SIGTRAP);
    }
#endif
}

// OFFSET: 0x00439880
void Blizzard::Debug::DefaultCallback(const char* expression, const char* message, const char* fileName, uint32_t lineNumber) {
    char text[2048];
#if defined(_WIN32)
    char path[512];
#endif
    char canonical[256];
    char directory[512];
    Blizzard::Time::TimeRec rec;

    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);
    Blizzard::String::Format(text, sizeof(text), "%04d/%02d/%02d %02d:%02d:%02d  %s(%d) : Assertion failed: %s  \"%s\"\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, CanonicalStr(fileName, canonical, sizeof(canonical)), lineNumber, expression, message);
    fprintf(stderr, "%s", text);

#if defined(_WIN32)
    Blizzard::Process::GetProcessDirectory(directory, sizeof(directory));
#else
    // Never fall back to a relative or truncated log destination.
    if (Blizzard::Process::GetProcessDirectory(directory, sizeof(directory)) &&
        std::strlen(directory) + sizeof("Errors/Assert.log") <= sizeof(directory)) {
#endif
    Blizzard::String::Append(directory, "Errors", sizeof(directory));
#if defined(_WIN32)
    Blizzard::File::CreateDirectory(directory, true);
#else
    System_Debug::Native::CreateDirectory(directory);
#endif
    Blizzard::String::Append(directory, "/Assert.log", sizeof(directory));
#if defined(_WIN32)
    Blizzard::String::MakeBackslashPath(directory, path, sizeof(path));
#endif

    FILE* stream = nullptr;
#if defined(_WIN32)
    fopen_s(&stream, path, "a");
#else
    stream = System_Debug::Native::OpenAppend(directory);
#endif

    if (stream) {
        fprintf(stream, "%s", text);
        fclose(stream);
    }

#if !defined(_WIN32)
    }
#endif

    Breakpoint();

    if (!s_continueAfterAssert) {
        abort();
    }
}

// OFFSET: 0x004321B0
void System_Debug::ShutdownProc() {
#if defined(_WIN32)
    if (s_dbgHelpInitialized && s_dbgHelp) {
        Blizzard::Lock::MutexEnter(s_dbgHelpLock);

        if (s_dbgHelp) {
            FreeLibrary(s_dbgHelp);
            s_dbgHelp = nullptr;
        }

        Blizzard::Lock::MutexLeave(s_dbgHelpLock);
        Blizzard::Lock::MutexDestroy(s_dbgHelpLock);
    }
#else
    // No dbghelp module is initialized; every native diagnostic FILE is scoped
    // to its operation. Repeated shutdown therefore has no retained resources.
#endif
}

#endif
