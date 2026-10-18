#ifndef BLIZZARD_LOG_NATIVE_H
#define BLIZZARD_LOG_NATIVE_H
#include <cstdint>
#include <cstdio>
namespace System_Log {
int NativeHostname(char* name, uint32_t count);
bool NativeAcquire(int32_t logClass);
void NativeShutdown();
uint32_t NativeLogThread(void* param);
bool NativeIsAbsolutePath(const char* path);
bool NativeCreateDirectory(const char* path, bool recursive);
int NativeOpen(FILE** stream, const char* path, const char* mode);
}
#endif
