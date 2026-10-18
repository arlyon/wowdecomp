#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <limits>
#include <thread>
#include <vector>

using namespace Blizzard::Time;

namespace {
void Roundtrip(Timestamp stamp) {
    TimeRec rec{};
    BreakTime(stamp, rec);
    assert(rec.month >= 1 && rec.month <= 12);
    assert(rec.nanosecond >= 0 && rec.nanosecond < 1000000000);
    errno = 0;
    assert(MakeTime(rec) == stamp);
    assert(errno == 0);
}

void ExpectError(TimeRec rec, int error) {
    errno = 0;
    assert(MakeTime(rec) == std::numeric_limits<Timestamp>::min());
    assert(errno == error);
}
}

int main() {
    // Race first use of both clocks; each thread must observe monotonic results.
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            while (!start.load()) { std::this_thread::yield(); }
            auto previous = GetTimestamp();
            auto previousMs = Milliseconds();
            for (int j = 0; j < 10000; ++j) {
                auto now = GetTimestamp();
                auto ms = Milliseconds();
                assert(now >= previous && ms >= previousMs);
                previous = now;
                previousMs = ms;
                if (j % 100 == 0) { Roundtrip(now); }
            }
        });
    }
    start = true;
    for (auto& thread : threads) { thread.join(); }

    TimeRec epoch{};
    BreakTime(0, epoch);
    assert(epoch.year == 2000 && epoch.month == 1 && epoch.day == 1);
    assert(epoch.hour == 0 && epoch.minute == 0 && epoch.second == 0 && epoch.nanosecond == 0);
    assert(epoch.weekday == 6 && epoch.yearday == 0);
    TimeRec before{};
    BreakTime(-1, before);
    assert(before.year == 1999 && before.month == 12 && before.day == 31);
    assert(before.hour == 23 && before.minute == 59 && before.second == 59);
    assert(before.nanosecond == 999999999 && before.weekday == 5 && before.yearday == 364);

    for (Timestamp value : {std::numeric_limits<Timestamp>::min(), std::numeric_limits<Timestamp>::min() + 1,
                            Timestamp(-1000000001), Timestamp(-100), Timestamp(-1), Timestamp(0), Timestamp(1),
                            Timestamp(99), Timestamp(1000000001), std::numeric_limits<Timestamp>::max()}) {
        Roundtrip(value);
    }
    uint64_t random = 0x123456789abcdef;
    for (int i = 0; i < 50000; ++i) {
        random = random * 6364136223846793005ULL + 1;
        Roundtrip(static_cast<int64_t>(random));
    }
    for (int year : {1708, 1800, 1900, 1969, 2000, 2004, 2100, 2200, 2291}) {
        for (int month = 1; month <= 12; ++month) {
            TimeRec rec{year, month, 1, 12, 34, 56, 987654321, -123, -456};
            errno = 0;
            auto stamp = MakeTime(rec);
            assert(errno == 0);
            TimeRec result{};
            BreakTime(stamp, result);
            assert(result.year == year && result.month == month && result.day == 1);
            assert(result.hour == 12 && result.minute == 34 && result.second == 56 && result.nanosecond == 987654321);
        }
    }
    TimeRec leap{2000, 2, 29, 0, 0, 0, 0, 0, 0};
    TimeRec result{};
    BreakTime(MakeTime(leap), result);
    assert(result.yearday == 59 && result.weekday == 2);
    leap.year = 1900; ExpectError(leap, EINVAL);
    leap.year = 2100; ExpectError(leap, EINVAL);
    leap.year = 2004; assert(MakeTime(leap) > 0);
    TimeRec july{2001, 7, 31, 0, 0, 0, 0, 0, 0};
    BreakTime(MakeTime(july), result);
    assert(result.day == 31 && result.yearday == 212); // Preserve the original cumulative-day table.
    TimeRec june{2001, 6, 31, 0, 0, 0, 0, 0, 0};
    ExpectError(june, EINVAL);
    TimeRec unixMinusOne{1969, 12, 31, 23, 59, 59, 0, 0, 0};
    assert(MakeTime(unixMinusOne) == -946684801000000000LL && errno == 0);

    for (int field = 0; field < 10; ++field) {
        TimeRec invalid = epoch;
        switch (field) {
        case 0: invalid.month = 0; break;
        case 1: invalid.month = 13; break;
        case 2: invalid.day = 0; break;
        case 3: invalid.day = 32; break;
        case 4: invalid.hour = 24; break;
        case 5: invalid.minute = 60; break;
        case 6: invalid.second = 60; break;
        case 7: invalid.hour = -1; break;
        case 8: invalid.minute = -1; break;
        case 9: invalid.second = -1; break;
        }
        ExpectError(invalid, EINVAL);
    }
    for (int ns : {-1, 1000000000, std::numeric_limits<int>::max()}) {
        TimeRec invalid = epoch; invalid.nanosecond = ns; ExpectError(invalid, EINVAL);
    }
    for (int year : {std::numeric_limits<int>::min(), 1601, 1707, 2293, std::numeric_limits<int>::max()}) {
        TimeRec outside = epoch; outside.year = year; ExpectError(outside, ERANGE);
    }
    TimeRec edge{};
    BreakTime(std::numeric_limits<Timestamp>::min(), edge);
    errno = EINVAL;
    assert(MakeTime(edge) == std::numeric_limits<Timestamp>::min() && errno == 0);
    BreakTime(std::numeric_limits<Timestamp>::max(), edge);
    ++edge.nanosecond; ExpectError(edge, ERANGE);
    BreakTime(std::numeric_limits<Timestamp>::min(), edge);
    --edge.nanosecond; ExpectError(edge, ERANGE);

    timespec wall{};
    clock_gettime(CLOCK_REALTIME, &wall);
    const auto expected = (wall.tv_sec - 946684800LL) * 1000000000LL + wall.tv_nsec;
    const auto observed = GetTimestamp();
    assert(observed > expected - 2000000000LL && observed < expected + 2000000000LL);
    timespec monotonic{};
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    const auto expectedMs = monotonic.tv_sec * 1000LL + monotonic.tv_nsec / 1000000;
    assert(Milliseconds() >= expectedMs && Milliseconds() <= expectedMs + 2000);
    const auto startNs = GetTimestamp();
    const auto startMs = Milliseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(GetTimestamp() - startNs >= 20000000);
    assert(Milliseconds() - startMs >= 20);
}
