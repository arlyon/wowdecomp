#ifndef ENGINE_SOURCE_BLIZZARDCORE_SFILE2_IO_H
#define ENGINE_SOURCE_BLIZZARDCORE_SFILE2_IO_H

#if defined(_WIN32)
// Keep the original Windows include surface for source-matching builds.
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#else
#include <cstdint>
#include "Engine/Source/Storm/H/Types.h"

class SFileImpl;
struct SOVERLAPPED;

// Renderer shader loading only needs opaque handles, not archive internals.
int32_t STORMAPI SFileCloseFile(SFileImpl* file);
int32_t STORMAPI SFileOpenFile(const char* name, SFileImpl** result);
int32_t STORMAPI SFileReadFile(SFileImpl* file, void* buffer, uint32_t count, uint32_t* bytesRead, SOVERLAPPED* overlapped, uint32_t unused);
#endif

#endif
