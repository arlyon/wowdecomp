#include "Engine/Source/Storm/H/storm.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int failures;
int headers;
void Check(bool condition, const char* text) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", text); ++failures; }
}
int32_t STORMAPI Header(char* buffer, uint32_t size) {
    ++headers; std::snprintf(buffer, size, "owned test header\n"); return 1;
}
int32_t STORMAPI Recover(uint32_t, const char*, const char*, int32_t, const char*) { return 0; }
}
int main() {
    char fixture[] = "/tmp/storm-services-XXXXXX";
    char* root = mkdtemp(fixture);
    if (!root) return 1;
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("HOME", root, 1);
    uint32_t value = 99;
    Check(!SRegLoadValue("missing", "v", 0, &value) && value == 99 && SErrGetLastError() == 2, "missing setting");
    Check(SRegSaveString("Key\\../path", "Name", 0, "line1\nline2\tUTF8:\xc3\xa9"), "save escaped UTF8 string");
    char text[256]{};
    Check(SRegLoadString("KEY\\../PATH", "NAME", 0, text, sizeof(text)) && !std::strcmp(text, "line1\nline2\tUTF8:\xc3\xa9"), "case insensitive roundtrip");
    Check(!SRegLoadString("Key\\../path", "Name", 0, text, 2) && SErrGetLastError() == 234, "short buffer");
    Check(SRegSaveValue("Key", "v", SREG_FLUSH_KEY, UINT32_MAX), "save uint32");
    Check(SRegLoadString("Key", "v", 0, text, sizeof(text)) && !std::strcmp(text, "4294967295"), "typed numeric string load");
    Check(SRegSaveString("Key", "hex", 0, "0xff") && SRegLoadValue("Key", "hex", 0, &value) && value == 255, "parse string value");
    Check(SRegSaveString("Key", "bad", 0, "9junk") && !SRegLoadValue("Key", "bad", 0, &value), "reject malformed number");
    Check(!SRegSaveValue("Key", "v", SREG_EXCLUDE_CURRENT_USER, 1) && SErrGetLastError() == 5, "machine-only denied");
    Check(SRegSaveValue("Key", "v", SREG_BATTLE_NET, 7), "battle namespace save");
    Check(SRegLoadValue("Key", "v", 0, &value) && value == UINT32_MAX, "namespace isolation");
    Check(!SRegSaveString(nullptr, "v", 0, "x"), "invalid argument");
    Check(SRegSaveValue("Software\\Blizzard Entertainment\\Key", "v", SREG_ABSOLUTE, 12), "absolute namespace save");
    Check(SRegLoadValue("Key", "v", 0, &value) && value == 12, "absolute key matches expanded relative key");
    const std::string first(10000, 'a'), second(12000, 'b');
    Check(SRegSaveString("atomic", "v", 0, first.c_str()), "seed atomic value");
    std::atomic<bool> done{false};
    std::atomic<bool> complete{true};
    std::thread reader([&] {
        char snapshot[13000];
        do {
            if (!SRegLoadString("atomic", "v", 0, snapshot, sizeof(snapshot)) ||
                (snapshot != first && snapshot != second)) complete = false;
        } while (!done.load());
    });
    for (int i = 0; i < 30; ++i) Check(SRegSaveString("atomic", "v", 0, (i % 2 ? first : second).c_str()), "replace atomic value");
    done = true;
    reader.join();
    Check(complete, "concurrent readers see only complete replacements");
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) writers.emplace_back([t] {
        for (int i = 0; i < 30; ++i) {
            if (!SRegSaveValue("threads", std::to_string(t).c_str(), 0, i)) std::abort();
        }
    });
    for (auto& thread : writers) thread.join();
    for (int t = 0; t < 4; ++t) Check(SRegLoadValue("threads", std::to_string(t).c_str(), 0, &value) && value == 29, "thread updates retained");
    pid_t children[2];
    for (int p = 0; p < 2; ++p) {
        children[p] = fork();
        if (!children[p]) {
            uint32_t persisted = 0;
            if (!SRegLoadValue("Key", "v", 0, &persisted) || persisted != 12) _exit(3);
            for (int i = 0; i < 30; ++i) if (!SRegSaveValue("process", std::to_string(p).c_str(), 0, i)) _exit(2);
            _exit(0);
        }
        Check(children[p] > 0, "fork fixture");
    }
    for (pid_t child : children) if (child > 0) { int status; Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "process persistence"); }
    for (int p = 0; p < 2; ++p) Check(SRegLoadValue("process", std::to_string(p).c_str(), 0, &value) && value == 29, "cross-process updates retained");
    // Corrupt a generated entry; parsing must fail rather than invent a value.
    for (const auto& file : std::filesystem::directory_iterator(std::filesystem::path(root) / "wowdecomp/storm")) {
        if (file.path().filename().string()[0] != '.') std::ofstream(file.path()) << "corrupt";
    }
    Check(!SRegLoadValue("Key", "v", 0, &value), "corrupt setting rejected");
    const auto deniedRoot = std::filesystem::path(root) / "not-a-directory";
    std::ofstream(deniedRoot) << "fixture";
    setenv("XDG_CONFIG_HOME", deniedRoot.c_str(), 1);
    Check(!SRegSaveValue("key", "v", 0, 1), "filesystem persistence error reported");
    setenv("XDG_CONFIG_HOME", root, 1);
    auto prior = std::get_terminate();
    SErrCatchUnhandledExceptions();
    auto installed = std::get_terminate();
    Check(installed != prior, "terminate handler installed");
    SErrCatchUnhandledExceptions();
    Check(std::get_terminate() == installed, "terminate install idempotent");
    pid_t child = fork();
    if (!child) { rlimit limit{0, 0}; setrlimit(RLIMIT_CORE, &limit); std::terminate(); }
    if (child > 0) { int status; Check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "terminate aborts owned child"); }
    else Check(false, "fork terminate child");
    SErrInitialize();
    SErrSetLogTitleString("owned test title");
    SErrSetLogCallback(Header);
    SErrRegisterHandler(Recover);
    Check(SErrDisplayError(1, __FILE__, __LINE__, "test diagnostic", 1, 1, 0) == 1 && headers == 1, "real diagnostic header callback");
    Check(!SErrGetLogLastPath(text, sizeof(text)), "no invented crash path");
    Check(SErrDestroy() && std::get_terminate() == prior, "terminate handler restored");
    Check(!SCreateProcess("Launcher.exe", nullptr, nullptr, nullptr) && SErrGetLastError() == 50, "explicit unsupported launch, no process created");
    Check(!SCreateProcess(nullptr, nullptr, nullptr, nullptr) && SErrGetLastError() == 87, "invalid process request");
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* mapping = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(mapping != MAP_FAILED, "owned mapping");
    if (mapping != MAP_FAILED) {
        Check(SMemIsValidPointer(mapping, 2 * page, 0), "readable range");
        Check(!mprotect(static_cast<char*>(mapping) + page, page, PROT_NONE), "protect fixture");
        Check(!SMemIsValidPointer(mapping, 2 * page, 0), "unreadable tail rejected");
        munmap(mapping, 2 * page);
        Check(!SMemIsValidPointer(mapping, 1, 0), "unmapped range rejected");
    }
    Check(!SMemIsValidPointer(nullptr, 1, 0) && !SMemIsValidPointer(reinterpret_cast<void*>(UINTPTR_MAX - 2), 8, 0), "null and overflow rejected");
    SLogInitialize();
    HSLOG log{};
    std::string logPath = std::string(root) + "/owned.log";
    Check(SLogCreate(logPath.c_str(), SLOG_FLAG_OPENFILE, &log), "owned log");
    SLogInitialize();
    SLogWrite(log, "before destroy");
    SLogDestroy();
    Check(std::filesystem::file_size(logPath) > 0, "log init preserves live handles");
    StormInitialize(); StormInitialize(); StormDestroy(); StormDestroy();
    StormInitialize(); StormDestroy();
    std::filesystem::remove_all(root);
    std::printf("Storm services: %d failures\n", failures);
    return failures ? 1 : 0;
}
