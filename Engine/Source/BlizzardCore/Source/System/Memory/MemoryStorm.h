#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_MEMORY_MEMORYSTORM_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_MEMORY_MEMORYSTORM_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Storm/H/Compiler.h"

namespace Blizzard {
namespace Memory {

void* Allocate(uint32_t bytes);
void* Allocate(uint32_t bytes, uint32_t flags, const char* fileName, int32_t lineNumber);
void Free(void* ptr);
void Free(void* ptr, uint32_t flags, const char* fileName, int32_t lineNumber);

}
}

namespace System_Memory {

class UsingMemory {
    public:
    UsingMemory();
    ~UsingMemory();
};

void ShutdownProc();

}

void* operator new(size_t bytes, uint32_t flags, const char* fileName, int32_t lineNumber);
void operator delete(void* ptr, uint32_t flags, const char* fileName, int32_t lineNumber);

#endif
