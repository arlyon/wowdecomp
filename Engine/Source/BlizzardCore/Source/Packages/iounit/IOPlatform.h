#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_PACKAGES_IOUNIT_IOPLATFORM_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_PACKAGES_IOUNIT_IOPLATFORM_H

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdint>

namespace System_File::Native {
bool GetFileAlignment(const char* path, uint32_t& alignment);
bool ReadUnbuffered(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead);
}
#endif

#endif
