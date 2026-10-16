#include <atomic>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <pthread.h>
#include <regex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

namespace {
[[noreturn]] void FailCheck(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    std::abort();
}
}

// Evaluate exactly once in every build configuration, including with NDEBUG.
#define CHECK(expression) \
    do { if (!(expression)) FailCheck(#expression, __FILE__, __LINE__); } while (false)

std::atomic<bool> failCreate{true};
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*f)(void*), void* p) {
    if (failCreate.exchange(false)) return EAGAIN;
    return __real_pthread_create(t, a, f, p);
}
std::string Read(const std::filesystem::path& p) {
    std::ifstream input(p);
    return {std::istreambuf_iterator<char>(input), {}};
}
int main() {
    // All output, including the default executable-relative directory, is in a
    // generated fixture. The harness copies this binary before execution.
    char fixture[] = "/tmp/blizzard-log-XXXXXX";
    CHECK(mkdtemp(fixture));
    const std::string root = std::string(fixture) + "/literal\\directory/";
    CHECK(!System_Log::Acquire(0));
    CHECK(!System_Log::s_state && !System_Log::s_thread && !System_Log::s_threadRunning);
    std::vector<std::thread> writers;
    for (int i = 0; i < 12; ++i) writers.emplace_back([&] {
        CHECK(System_Log::Acquire(0));
        Blizzard::Log::Printf(0, (root + "first.log").c_str(), "first writer\n");
        // Verify recursive admission and release without an intervening shutdown.
        CHECK(System_Log::Acquire(0));
        Blizzard::Lock::MutexLeave(System_Log::s_mutex);
        Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    });
    for (auto& t : writers) t.join();
    Blizzard::Log::Flush(0);
    CHECK(Read(root + "first.log").find("first writer") != std::string::npos);
    CHECK(System_Log::Acquire(0));
    char executableDirectory[1024];
    ssize_t n = readlink("/proc/self/exe", executableDirectory, sizeof(executableDirectory) - 1);
    CHECK(n > 0);
    executableDirectory[n] = '\0';
    *strrchr(executableDirectory, '/') = '\0';
    std::string executableBase = std::string(executableDirectory) + "/";
    CHECK(executableBase == System_Log::s_Prefs[0].m_directory);
    free(System_Log::s_Prefs[0].m_directory);
    for (int i = 0; i < 4; ++i) System_Log::s_Prefs[i].m_directory = nullptr;
    System_Log::s_Prefs[0].m_directory = strdup(root.c_str());
    Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    for (int i = 0; i < 12; ++i) writers[i] = std::thread([i] {
        Blizzard::Log::Printf(0, "test\\name.log", "writer %d\n", i);
    });
    for (auto& t : writers) t.join();
    Blizzard::Log::Printf(3, "disabled.log", "invisible\n");
    Blizzard::Log::Flush(0);
    auto text = Read(root + "test\\name.log");
    CHECK(text.find("# System started at ") != std::string::npos);
    CHECK(text.find("# system: ") != std::string::npos);
    CHECK(std::regex_search(text, std::regex("[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{4} writer")));
    for (int i = 0; i < 12; ++i) CHECK(text.find("writer " + std::to_string(i) + "\n") != std::string::npos);
    CHECK(!std::filesystem::exists(root + "disabled.log"));
    Blizzard::Log::Printf(0, "test\\name.log", "appended\n");
    Blizzard::Log::Flush(0);
    CHECK(Read(root + "test\\name.log").find(text) == 0);
    std::filesystem::remove(root + "test\\name.log");
    Blizzard::Log::Printf(0, "test\\name.log", "rotated\n");
    Blizzard::Log::Flush(0);
    auto rotated = Read(root + "test\\name.log");
    CHECK(rotated.find("# system: ") != std::string::npos);
    CHECK(rotated.find("# System started at ") == std::string::npos);
    Blizzard::Log::Printf(1, "max-open.log", "max-open\n");
    Blizzard::Thread::Sleep(5);
    CHECK(System_Log::Acquire(1));
    System_Log::FlushLogs(false);
    for (const auto& log : System_Log::s_logs)
        if (log.m_name && std::string(log.m_name) == "max-open.log") CHECK(!log.m_file);
    char expanded[1024];
    CHECK(Blizzard::Log::Expand(1, "dated-%Y-%T.log", expanded, sizeof(expanded), false));
    CHECK(std::string(expanded).find('%') == std::string::npos);
    Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    CHECK(Read(root + "max-open.log").find("max-open") != std::string::npos);
    CHECK(System_Log::Acquire(4));
    System_Log::s_Prefs[4].m_noTimestamp = true;
    System_Log::s_Prefs[4].m_maxIdleTime = 1;
    Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    Blizzard::Log::Printf(4, "plain.log", "plain\n");
    Blizzard::Thread::Sleep(5);
    CHECK(System_Log::Acquire(4));
    System_Log::FlushLogs(false);
    for (const auto& log : System_Log::s_logs) if (log.m_name && std::string(log.m_name) == "plain.log") CHECK(!log.m_file);
    Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    CHECK(Read(root + "plain.log").find("\nplain\n") != std::string::npos);
    Blizzard::Log::Printf(4, "worker.log", "worker closes this\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    bool closed = false;
    do {
        Blizzard::Thread::Sleep(50);
        CHECK(System_Log::Acquire(4));
        for (const auto& log : System_Log::s_logs)
            if (log.m_name && std::string(log.m_name) == "worker.log") closed = !log.m_file;
        Blizzard::Lock::MutexLeave(System_Log::s_mutex);
    } while (!closed && std::chrono::steady_clock::now() < deadline);
    CHECK(closed);
    CHECK(Read(root + "worker.log").find("worker closes this") != std::string::npos);
    std::ofstream(root + "blocker") << "not a directory";
    Blizzard::Log::Printf(0, "blocker/failed.log", "not written\n");
    CHECK(!std::filesystem::exists(root + "blocker/failed.log"));
    std::atomic<bool> run{true};
    std::thread active([&] { while (run) Blizzard::Log::Printf(0, "racing.log", "race\n"); });
    auto start = std::chrono::steady_clock::now();
    System_Log::ShutdownProc();
    run = false;
    active.join();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
    CHECK(System_Log::s_threadDone && !System_Log::s_threadRunning && !System_Log::s_thread);
    CHECK(!System_Log::Acquire(0));
    Blizzard::Log::Printf(0, "after.log", "no\n");
    CHECK(!std::filesystem::exists(root + "after.log"));
    System_Log::ShutdownProc();
    System_Thread::ShutdownNativeThreads();
    std::filesystem::remove_all(fixture);
}
