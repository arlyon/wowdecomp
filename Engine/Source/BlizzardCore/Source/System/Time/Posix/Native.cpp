#if !defined(_WIN32)

#include <cerrno>
#include <ctime>
#include <limits>
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Native.h"

namespace {
constexpr int64_t NanosecondsPerSecond = 1000000000;
constexpr int64_t UnixToBlizzardSeconds = 946684800;
constexpr int64_t Failure = std::numeric_limits<int64_t>::min();
static_assert(sizeof(time_t) >= sizeof(int64_t), "Native time requires 64-bit time_t");

int64_t CheckedTimestamp(__int128 value) {
    if (value < std::numeric_limits<int64_t>::min() || value > std::numeric_limits<int64_t>::max()) {
        errno = ERANGE;
        return Failure;
    }
    return static_cast<int64_t>(value);
}

__int128 Nanoseconds(const timespec& value) {
    return static_cast<__int128>(value.tv_sec) * NanosecondsPerSecond + value.tv_nsec;
}

struct ClockAnchor {
    timespec monotonic{};
    timespec realtime{};
    int error = 0;

    ClockAnchor() {
        // Like the performance-counter path, wall time is sampled once and then
        // advanced only by the monotonic clock, independent of wall-clock edits.
        if (clock_gettime(CLOCK_MONOTONIC, &monotonic) || clock_gettime(CLOCK_REALTIME, &realtime)) {
            error = errno;
        }
    }
};
}

void System_Time::NativeBreakTime(Blizzard::Time::Timestamp time, Blizzard::Time::TimeRec& rec) {
    int64_t seconds = time / NanosecondsPerSecond;
    if (time % NanosecondsPerSecond < 0) {
        --seconds;
    }
    time_t unixSeconds = seconds + UnixToBlizzardSeconds;
    tm calendar{};
    gmtime_r(&unixSeconds, &calendar); // All int64 nanosecond timestamps fit time_t and tm.
    rec.year = calendar.tm_year + 1900;
    rec.month = calendar.tm_mon + 1;
    rec.day = calendar.tm_mday;
    rec.hour = calendar.tm_hour;
    rec.minute = calendar.tm_min;
    rec.second = calendar.tm_sec;
    rec.weekday = calendar.tm_wday;
}

Blizzard::Time::Timestamp System_Time::NativeMakeTime(const Blizzard::Time::TimeRec& rec) {
    const bool leap = !(rec.year % 400) || (rec.year % 100 && !(rec.year % 4));
    const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (rec.month < 1 || rec.month > 12 || rec.day < 1 ||
        rec.day > daysInMonth[rec.month - 1] + (leap && rec.month == 2) ||
        rec.hour < 0 || rec.hour > 23 || rec.minute < 0 || rec.minute > 59 ||
        rec.second < 0 || rec.second > 59 || rec.nanosecond < 0 || rec.nanosecond >= NanosecondsPerSecond) {
        errno = EINVAL;
        return Failure;
    }

    // Validate first so invalid fields remain EINVAL even on far-away dates.
    // These generous year bounds enclose every representable Timestamp.
    if (rec.year < 1707 || rec.year > 2292) {
        errno = ERANGE;
        return Failure;
    }
    tm calendar{};
    calendar.tm_year = rec.year - 1900;
    calendar.tm_mon = rec.month - 1;
    calendar.tm_mday = rec.day;
    calendar.tm_hour = rec.hour;
    calendar.tm_min = rec.minute;
    calendar.tm_sec = rec.second;
    errno = 0;
    const time_t seconds = timegm(&calendar);
    if (errno) {
        return Failure;
    }
    // timegm returning -1 is valid (1969-12-31 23:59:59 UTC).
    return CheckedTimestamp((static_cast<__int128>(seconds) - UnixToBlizzardSeconds) * NanosecondsPerSecond + rec.nanosecond);
}

Blizzard::Time::Timestamp System_Time::NativeTimestamp() {
    static const ClockAnchor anchor;
    if (anchor.error) {
        errno = anchor.error;
        return Failure;
    }
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return Failure;
    }
    return CheckedTimestamp(Nanoseconds(anchor.realtime) - static_cast<__int128>(UnixToBlizzardSeconds) * NanosecondsPerSecond +
                            Nanoseconds(now) - Nanoseconds(anchor.monotonic));
}

int64_t System_Time::NativeMilliseconds() {
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        return Failure;
    }
    return CheckedTimestamp(static_cast<__int128>(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
}

#endif
