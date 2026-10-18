#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_DEBUG_NATIVE_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_DEBUG_NATIVE_H

#include <cstdio>

// Native leaves used by shared assertion reporting. Implementations belong to
// the selected OS provider; this contract does not choose Linux or Darwin.
namespace System_Debug::Native {
bool DebuggerPresent();
void CreateDirectory(const char* directory);
FILE* OpenAppend(const char* path);
}

#endif
