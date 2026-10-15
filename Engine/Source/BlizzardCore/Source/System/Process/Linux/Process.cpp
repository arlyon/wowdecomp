#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"

bool Blizzard::Process::GetProcessPath(char* buffer, int32_t size) {
    if (!buffer || size <= 0) { errno = EINVAL; return false; }
    const ssize_t count = readlink("/proc/self/exe", buffer, static_cast<size_t>(size));
    if (count < 0) { buffer[0] = 0; return false; }
    if (count >= size) { buffer[0] = 0; errno = ENAMETOOLONG; return false; }
    buffer[count] = 0;
    return true;
}

bool Blizzard::Process::GetProcessDirectory(char* buffer, int32_t size) {
    if (!GetProcessPath(buffer, size)) return false;
    char* slash = std::strrchr(buffer, '/');
    if (!slash) { buffer[0] = 0; errno = EINVAL; return false; }
    slash[1] = 0;
    return true;
}

void Blizzard::Process::Sleep(uint32_t milliseconds) {
    if (!milliseconds) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
