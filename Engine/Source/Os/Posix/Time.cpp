#include <ctime>
#include "Engine/Source/Os/Time.h"

int32_t OsGetTimeZone() {
    // POSIX timezone is seconds west of UTC, excluding daylight saving time.
    // Windows returns the standard-time bias too; DST is handled by callers.
    tzset();
    return static_cast<int32_t>(-::timezone / 60);
}

void OsGetTimeStamp(char* buffer, uint32_t size) {
    if (!buffer || !size) {
        return;
    }
    buffer[0] = '\0';
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) && !std::strftime(buffer, size, "%m%d%y_%H%M%S", &local)) {
        buffer[0] = '\0';
    }
}
