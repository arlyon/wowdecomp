#include "Engine/Source/Os/OsLibrary.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Tls.h"
#include "Engine/Source/Storm/H/SCritSect.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
int main(int argc, char** argv) {
    Check(argc == 2, "fixture path supplied");
    const auto before = OsGetAsyncTimeClocks();
    Check(OsGetAsyncClocksPerSecond() > 0, "clock frequency");
    OsSleep(1);
    Check(OsGetAsyncTimeClocks() >= before, "monotonic clock after sleep");
    const auto key = OsTlsAlloc();
    Check(key != std::numeric_limits<uint32_t>::max(), "allocate TLS");
    int mainValue = 1;
    Check(OsTlsSetValue(key, &mainValue), "main-thread TLS");
    CCritSect lock;
    unsigned counter = 0;
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        int value = 2;
        if (OsTlsGetValue(key) || !OsTlsSetValue(key, &value)) valid = false;
        for (unsigned j = 0; j < 1000; ++j) {
            lock.Enter(); lock.Enter();
            ++counter;
            lock.Leave(); lock.Leave();
            if (OsTlsGetValue(key) != &value) valid = false;
        }
    });
    for (auto& worker : workers) worker.join();
    Check(valid && counter == 8000, "recursive locking and isolated TLS");
    Check(OsTlsGetValue(key) == &mainValue, "main TLS unchanged by workers");
    OsTlsFree(key);
    errno = 0;
    Check(!OsTlsSetValue(std::numeric_limits<uint32_t>::max(), nullptr) && errno == EINVAL,
        "invalid TLS key rejected");
    Check(!OsLoadLibrary(nullptr) && errno == EINVAL, "invalid library path rejected");
    void* library = OsLoadLibrary(argv[1]);
    Check(library != nullptr, "load real shared-library fixture");
    auto value = reinterpret_cast<int (*)()>(OsGetLibraryEntryPoint(library, "PosixLibraryValue"));
    Check(value && value() == 42, "resolve and invoke exported function");
    Check(!OsGetLibraryEntryPoint(library, "missing_symbol"), "missing symbol rejected");
    OsFreeLibrary(library);
    return 0;
}
