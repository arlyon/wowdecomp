#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"

namespace {
unsigned failures;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
std::atomic<unsigned> constructions{0};
struct alignas(64) Value {
    unsigned marker = 42;
    Value() { ++constructions; }
};
}

int main() {
    using namespace Blizzard::Lock;
    Atomic counter;
    Check(counter.Increment() == 1 && counter.Increment() == 2 && counter.Decrement() == 1, "interlocked result semantics");
    RMutex lock;
    unsigned protectedCount = 0;
    std::array<std::thread, 8> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            ScopedMutex first(lock.m_mutex);
            ScopedMutex recursive(lock.m_mutex);
            ++protectedCount;
        }
    });
    for (auto& worker : workers) worker.join();
    Check(protectedCount == 8000, "recursive mutex excludes other threads");
    ConstructOnce<Value> once;
    std::atomic<bool> valid{true};
    for (auto& worker : workers) worker = std::thread([&] {
        Value* value = once.Get();
        if (value->marker != 42 || reinterpret_cast<uintptr_t>(value) % alignof(Value)) valid = false;
    });
    for (auto& worker : workers) worker.join();
    Check(valid && constructions == 1, "existing construct-once algorithm publishes one aligned object");
    Event request = nullptr, reply = nullptr;
    EventCreate(request);
    EventCreate(reply);
    Check(!EventWait(0, request), "unsignaled event times out");
    EventSet(request);
    EventSet(request);
    Check(EventWait(0, request) == &request && !EventWait(0, request), "auto-reset signals coalesce and are consumed once");
    std::atomic<unsigned> received{0};
    std::thread producer([&] {
        for (unsigned i = 0; i < 200; ++i) {
            EventWaitForever(request);
            ++received;
            EventSet(reply);
        }
    });
    for (unsigned i = 0; i < 200; ++i) {
        EventSet(request);
        EventWaitForever(reply);
    }
    producer.join();
    Check(received == 200, "event handshake does not lose notifications");
    EventDestroy(request);
    EventDestroy(reply);
    Check(!request && !reply && !EventWait(0, request) && errno == EINVAL, "destroyed event cannot be waited on");
    char path[4096], directory[4096];
    Check(Blizzard::Process::GetProcessPath(path, sizeof(path)) && std::filesystem::is_regular_file(path), "native executable path");
    Check(Blizzard::Process::GetProcessDirectory(directory, sizeof(directory)) && directory[std::strlen(directory)-1] == '/' &&
          std::filesystem::path(directory).lexically_normal() == std::filesystem::path(path).parent_path() / "",
          "process directory retains the original trailing separator contract");
    char small[1] = {'#'};
    Check(!Blizzard::Process::GetProcessPath(small, sizeof(small)) && !small[0] && errno == ENAMETOOLONG, "truncated process path is rejected");
    Check(!Blizzard::Process::GetProcessPath(nullptr, 0) && errno == EINVAL, "invalid process path buffer");
    Blizzard::Process::Sleep(0);
    if (!failures) std::puts("PASS: native recursive locks, atomics, auto-reset events, construct-once and process path");
    return failures ? 1 : 0;
}
