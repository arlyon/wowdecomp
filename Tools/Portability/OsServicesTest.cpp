#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Source/Os/OsLibrary.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Os/Tls.h"

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void CheckTiming() {
    OsGetAsyncTimeMs(); // Also exercise use before explicit startup.
    for (auto method : {OsTimeManager::BestAvailable, OsTimeManager::SystemMethod1,
                        OsTimeManager::SystemMethod2}) {
        OsTimeStartup(method);
        Check(OsTimeGetTestError() == 0, "native clock calibration");
        Check(OsTimeGetTimingMethod() == (method == OsTimeManager::SystemMethod1
            ? OsTimeManager::SystemMethod1 : OsTimeManager::SystemMethod2), "selected clock method");
        const int64_t frequency = OsGetAsyncClocksPerSecond();
        Check(frequency == (method == OsTimeManager::SystemMethod1 ? 1000 : 1000000000),
              "clock frequency matches clock units");
        const uint32_t before = OsGetAsyncTimeMs();
        const uint64_t clocks = OsGetAsyncTimeClocks();
        const uint32_t after = OsGetAsyncTimeMs();
        const uint32_t clockMs = static_cast<uint32_t>(clocks / (frequency / 1000));
        Check(clockMs - before <= after - before, "raw clocks and milliseconds share an epoch");
        OsSleep(15);
        Check(OsGetAsyncTimeMs() - before >= 10, "sleep advances monotonic milliseconds");
        Check(OsGetAsyncTimeClocks() >= clocks, "raw clocks are monotonic");
        Check(OsGetAsyncTimeSec() >= 0.0f, "seconds wrapper");
        OsSleep(0);
        OsTimeShutdown();
    }
    OsTimeStartup(OsTimeManager::BestAvailable);
    Check(OsTimeGetTimingMethod() == OsTimeManager::SystemMethod2, "restart after shutdown");
    OsTimeShutdown();

    std::array<char, 16> stamp;
    stamp.fill('x');
    OsGetTimeStamp(stamp.data(), 14);
    Check(stamp[13] == '\0' && stamp[6] == '_', "timestamp format and termination");
    for (unsigned i = 0; i < 13; ++i) {
        Check(i == 6 || std::isdigit(static_cast<unsigned char>(stamp[i])), "timestamp digits");
    }
    Check(stamp[14] == 'x', "timestamp respects buffer capacity");
    char small[3] = {'x', 'x', 'x'};
    OsGetTimeStamp(small, 1);
    Check(small[0] == '\0' && small[1] == 'x', "short timestamp is an empty string");
    small[0] = 'x';
    OsGetTimeStamp(small, 0);
    Check(small[0] == 'x', "zero-capacity timestamp does not write");
    OsGetTimeStamp(nullptr, 0);
}

void CheckTimeZones() {
    const char* previous = std::getenv("TZ");
    const bool hadPrevious = previous != nullptr;
    const std::string saved = previous ? previous : "";
    struct Zone { const char* name; int32_t minutes; };
    for (const Zone& zone : {Zone{"UTC0", 0}, Zone{"EST5EDT", -300},
                             Zone{"NST3:30NDT", -210}, Zone{"LHST-10:30LHDT-11", 630}}) {
        Check(setenv("TZ", zone.name, 1) == 0, "set process-local test timezone");
        Check(OsGetTimeZone() == zone.minutes, "standard timezone offset excludes DST");
    }
    if (hadPrevious) {
        Check(setenv("TZ", saved.c_str(), 1) == 0, "restore timezone");
    } else {
        Check(unsetenv("TZ") == 0, "restore unset timezone");
    }
    tzset();
}

void CheckTls() {
    const uint32_t key = OsTlsAlloc();
    Check(key != UINT32_MAX, "TLS key allocation");
    if (key == UINT32_MAX) {
        return;
    }
    int owner = 42;
    Check(OsTlsGetValue(key) == nullptr, "new TLS key starts empty");
    Check(OsTlsSetValue(key, &owner) != 0, "set main-thread TLS");
    std::atomic<bool> isolated{true};
    std::array<int, 8> values{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < values.size(); ++i) {
        threads.emplace_back([&, i] {
            if (OsTlsGetValue(key) != nullptr || !OsTlsSetValue(key, &values[i])) {
                isolated = false;
            }
            for (unsigned repeat = 0; repeat < 1000; ++repeat) {
                std::this_thread::yield();
                if (OsTlsGetValue(key) != &values[i]) {
                    isolated = false;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    Check(isolated && OsTlsGetValue(key) == &owner, "TLS values are isolated across threads");
    Check(OsTlsSetValue(key, nullptr) != 0 && OsTlsGetValue(key) == nullptr, "clear TLS value");
    OsTlsFree(key);
    const uint32_t fresh = OsTlsAlloc();
    Check(fresh != UINT32_MAX && OsTlsGetValue(fresh) == nullptr, "reallocated key has no stale value");
    if (fresh != UINT32_MAX) {
        OsTlsFree(fresh);
    }
    errno = 0;
    Check(!OsTlsSetValue(UINT32_MAX, &owner) && errno == EINVAL, "invalid TLS sentinel is rejected");
    errno = 0;
    Check(OsTlsGetValue(UINT32_MAX) == nullptr && errno == EINVAL, "invalid TLS lookup is rejected");
    errno = 0;
    OsTlsFree(UINT32_MAX);
    Check(errno == EINVAL, "invalid TLS free is rejected");
}

void CheckLibraries(const char* fixture) {
    void* module = OsLoadLibrary(fixture);
    Check(module != nullptr, "load native shared library");
    if (module) {
        auto value = reinterpret_cast<int (*)()>(OsGetLibraryEntryPoint(module, "WowNativeTestValue"));
        Check(value != nullptr, "resolve exported function");
        if (value) {
            Check(value() == 321, "call resolved function");
        }
        Check(OsGetLibraryEntryPoint(module, "WowNativeTestMissingSymbol") == nullptr,
              "missing symbol returns failure");
        errno = 0;
        Check(OsGetLibraryEntryPoint(module, nullptr) == nullptr && errno == EINVAL,
              "null symbol name is rejected");
        OsFreeLibrary(module);
    }
    Check(OsLoadLibrary("/wowdecomp-test-missing/no-such-library.so") == nullptr,
          "missing library returns failure");
    errno = 0;
    Check(OsLoadLibrary(nullptr) == nullptr && errno == EINVAL, "null library name is rejected");
    errno = 0;
    Check(OsGetLibraryEntryPoint(nullptr, "symbol") == nullptr && errno == EINVAL,
          "null library handle is rejected");
    OsFreeLibrary(nullptr);
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <native-test-library>\n", argv[0]);
        return 2;
    }
    CheckTiming();
    CheckTimeZones();
    CheckTls();
    CheckLibraries(argv[1]);
    std::array<uint8_t, 64> random{};
    OsSecureRandom(random.data(), random.size());
    IOsSecureRandom(random.data(), random.size());
    OsSecureRandom(nullptr, 0);
    if (!failures) {
        std::puts("PASS: monotonic timing, timezones, TLS isolation, shared libraries and kernel randomness");
    }
    return failures ? 1 : 0;
}
