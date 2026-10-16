#include "Engine/Source/BlizzardCore/Source/System/Debug/Native.h"
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace System_Debug::Native {
bool DebuggerPresent() {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    kinfo_proc process{};
    size_t size = sizeof(process);
    return sysctl(mib, 4, &process, &size, nullptr, 0) == 0 &&
        (process.kp_proc.p_flag & P_TRACED) != 0;
}
void CreateDirectory(const char* path) { mkdir(path, 0777); }
FILE* OpenAppend(const char* path) { return std::fopen(path, "a"); }
}
