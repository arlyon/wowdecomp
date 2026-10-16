#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/Debug/Linux/Native.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void Require(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (errno=%d)\n", description, errno);
        std::exit(1);
    }
}

std::string Read(const fs::path& path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

size_t OpenFiles() {
    return static_cast<size_t>(std::distance(fs::directory_iterator("/proc/self/fd"),
                                           fs::directory_iterator()));
}

std::string Run(const fs::path& executable, const char* mode, const fs::path& workingDirectory) {
    int descriptors[2];
    Require(pipe(descriptors) == 0, "stderr pipe");
    pid_t child = fork();
    Require(child >= 0, "fork");
    if (!child) {
        rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        close(descriptors[0]);
        if (dup2(descriptors[1], STDERR_FILENO) < 0) { _exit(126); }
        close(descriptors[1]);
        if (chdir(workingDirectory.c_str()) != 0) { _exit(126); }
        execl(executable.c_str(), executable.c_str(), mode, nullptr);
        _exit(127);
    }
    close(descriptors[1]);
    Require(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == 0, "nonblocking stderr");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string output;
    int status = 0;
    for (;;) {
        char bytes[4096];
        ssize_t count;
        while ((count = read(descriptors[0], bytes, sizeof(bytes))) > 0) {
            output.append(bytes, static_cast<size_t>(count));
        }
        Require(count == 0 || errno == EAGAIN || errno == EINTR, "read stderr");
        pid_t result = waitpid(child, &status, WNOHANG);
        Require(result >= 0, "waitpid");
        if (result == child) {
            while ((count = read(descriptors[0], bytes, sizeof(bytes))) > 0) {
                output.append(bytes, static_cast<size_t>(count));
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            Require(false, "assert subprocess deadline");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    close(descriptors[0]);
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "assert terminates with SIGABRT, not SIGTRAP");
    Require(std::regex_match(output, std::regex(
        "[0-9]{4}/[0-9]{2}/[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}  Source/AssertFixture.cpp\\(731\\) : Assertion failed: native_expression  \\\"native message\\\"\n")),
        "timestamp, expression, message, canonical source and line on stderr");
    return output;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "--direct") == 0 || std::strcmp(argv[1], "--dispatch") == 0)) {
        // Exercise the actual SDK's canonical-prefix registry and default dispatch.
        const char prefix[] = "C:/Build/";
        std::memcpy(System_Debug::s_CanonBuffer, prefix, sizeof(prefix));
        System_Debug::s_CanonHead = System_Debug::s_CanonBuffer;
        System_Debug::s_CanonTail = System_Debug::s_CanonBuffer + sizeof(prefix);
        System_Debug::ShutdownProc();
        System_Debug::ShutdownProc();
        if (std::strcmp(argv[1], "--direct") == 0) {
            Blizzard::Debug::DefaultCallback("native_expression", "native message", "C:\\Build\\Source\\AssertFixture.cpp", 731);
        } else {
            Require(System_Debug::s_AssertCallback == Blizzard::Debug::DefaultCallback, "initial default callback");
            Blizzard::Debug::SetAssertHandler(Blizzard::Debug::DefaultCallback);
            System_Debug::s_AssertCallback("native_expression", "native message", "C:\\Build\\Source\\AssertFixture.cpp", 731);
        }
        return 99;
    }

    Require(argc == 2, "provide an owned output directory for fixtures");
    Require(!System_Debug::Native::DebuggerPresent(), "test must run without debugger attachment");
    const size_t descriptors = OpenFiles();
    for (int i = 0; i < 100; ++i) {
        System_Debug::ShutdownProc();
        Require(!System_Debug::Native::DebuggerPresent(), "untraced process detection");
        Blizzard::Debug::Breakpoint();
    }
    Require(OpenFiles() == descriptors, "idempotent no-resource shutdown and scoped debugger status files");

    fs::create_directories(argv[1]);
    std::string pattern = (fs::absolute(argv[1]) / "blizzard-assert-XXXXXX").string();
    Require(mkdtemp(pattern.data()) != nullptr, "owned temporary fixture directory");
    const fs::path fixture(pattern);
    const fs::path workingDirectory = fixture / "working";
    fs::create_directory(workingDirectory);
    const fs::path executable = fixture / "BlizzardAssertTest";
    fs::copy_file(fs::read_symlink("/proc/self/exe"), executable);
    fs::permissions(executable, fs::perms::owner_all);
    const fs::path log = fixture / "Errors" / "Assert.log";
    const std::string first = Run(executable, "--direct", workingDirectory);
    Require(fs::is_regular_file(log), "actual executable-relative Errors/Assert.log exists");
    Require(Read(log) == first, "first diagnostic persisted exactly");
    const std::string second = Run(executable, "--dispatch", workingDirectory);
    Require(Read(log) == first + second, "second diagnostic appended without truncation");
    fs::remove(log);
    fs::remove(log.parent_path());
    {
        std::ofstream blocked(log.parent_path());
        blocked << "ordinary file blocks Errors directory\n";
    }
    const std::string blocked = Read(log.parent_path());
    Run(executable, "--dispatch", workingDirectory);
    Require(Read(log.parent_path()) == blocked, "failed log destination unchanged; stderr and abort survive");
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator(fixture)) {
        (void)entry;
        ++files;
    }
    Require(files == 3, "no fallback log or alternate backslash filename");

    // Backslashes are literal native filename bytes, not path separators. Keep a
    // plausible translated destination present so a redirected append is caught.
    const fs::path nativeParent = fixture / "literal\\parent";
    const fs::path redirectedErrors = fixture / "literal" / "parent" / "Errors";
    fs::create_directory(nativeParent);
    fs::create_directories(redirectedErrors);
    const fs::path redirectedLog = redirectedErrors / "Assert.log";
    const std::string sentinel = "unrelated log must not be appended\n";
    {
        std::ofstream stream(redirectedLog);
        stream << sentinel;
    }
    const fs::path nativeExecutable = nativeParent / "BlizzardAssertTest";
    fs::copy_file(executable, nativeExecutable);
    fs::permissions(nativeExecutable, fs::perms::owner_all);
    const fs::path nativeLog = nativeParent / "Errors" / "Assert.log";
    const std::string nativeFirst = Run(nativeExecutable, "--dispatch", workingDirectory);
    Require(fs::is_regular_file(nativeLog), "literal-backslash executable-relative Errors/Assert.log exists");
    Require(Read(nativeLog) == nativeFirst, "literal-backslash diagnostic persisted exactly");
    const std::string nativeSecond = Run(nativeExecutable, "--dispatch", workingDirectory);
    Require(Read(nativeLog) == nativeFirst + nativeSecond, "literal-backslash diagnostic appended exactly");
    Require(Read(redirectedLog) == sentinel, "no redirected append after native path translation");
    Require(std::distance(fs::directory_iterator(redirectedErrors), fs::directory_iterator()) == 1,
            "no other redirected diagnostic writes");

    // A valid executable directory can still leave insufficient room for the log
    // suffix. Neither that case nor a failed GetProcessDirectory may log elsewhere.
    for (const size_t length : {size_t(499), size_t(600)}) {
        fs::path deep = fixture;
        Require(deep.string().size() + 2 < length, "output directory short enough for long-path fixture");
        while (deep.string().size() < length) {
            const size_t remaining = length - deep.string().size() - 1;
            deep /= std::string(remaining > 200 ? 200 : remaining, 'd');
        }
        fs::create_directories(deep);
        fs::copy_file(executable, deep / "a");
        fs::permissions(deep / "a", fs::perms::owner_all);
        Run(deep / "a", "--direct", workingDirectory);
        Require(std::distance(fs::directory_iterator(deep), fs::directory_iterator()) == 1,
                "long executable path creates no truncated or relative diagnostic log");
    }
    Require(fs::is_empty(workingDirectory), "no working-directory fallback log");
    fs::remove_all(fixture);
    std::puts("PASS: Blizzard default assertion callback, append/failure, canonical source, SIGABRT, resource-free shutdown");
}
