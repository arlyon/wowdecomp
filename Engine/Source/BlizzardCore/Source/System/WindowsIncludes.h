#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_WINDOWSINCLUDES_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_WINDOWSINCLUDES_H

// Keep Windows SDK inclusion at platform boundaries. Shared translation units
// retain their physical line numbers for allocator/assertion diagnostics.
#include "Engine/Source/Storm/H/Compiler.h"
#if defined(_WIN32)
#include <windows.h>
#define BLIZZARD_PLATFORM_SLEEP Sleep
#else
#define BLIZZARD_PLATFORM_SLEEP Blizzard::Process::Sleep
#endif

#endif
