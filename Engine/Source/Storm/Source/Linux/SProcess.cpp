#include "Engine/Source/Storm/H/storm.h"

int32_t STORMAPI SCreateProcess(const char* appName, char*, void (*)(void*), void*) {
    if (!appName || !*appName) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }
    // Current clients request Windows Launcher.exe/WowError.exe and a Windows
    // command line. There is no approved native executable/argument contract.
    SErrSetLastError(50); // ERROR_NOT_SUPPORTED
    return 0;
}
