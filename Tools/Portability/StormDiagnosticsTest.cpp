#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include "Engine/Source/Storm/H/storm.h"

namespace {
unsigned failures;
void Check(bool valid, const char* message) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Message { uint32_t code; const char* text; };
const Message messages[] = {
    {108, "Invalid or corrupt archive.\n"},
    {2, "The system cannot find the file specified.\r\n"},
    {8, "Not enough storage is available to process this command.\r\n"},
    {87, "The parameter is incorrect.\r\n"},
    {STORM_ERROR_ASSERTION, "Assertion Failure"},
    {STORM_ERROR_NOT_ARCHIVE, "Not Archive"},
    {STORM_ERROR_VERSION_MISMATCH, "Version Mismatch"},
    {STORM_ERROR_MEM_NOT_ALLOCATED, "Memory Already Freed"},
    {STORM_ERROR_MEM_CORRUPTED, "Memory Corrupt"},
    {STORM_ERROR_MEM_INVALID, "Memory Invalid Block"},
    {STORM_ERROR_MEM_NOT_FREED, "Memory Never Released"},
    {STORM_ERROR_RESOURCES_NOT_RELEASED, "Handle Never Released"},
    {STORM_ERROR_OUT_OF_BOUNDS, "Access Out Of Bounds"},
    {STORM_ERROR_FILE_CORRUPTED, "File Corrupt"},
    {STORM_ERROR_FATAL, "Fatal Exception"},
    {STORM_ERROR_FATAL_CONDITION, "Fatal Condition"}
};
void TestMessages() {
    char buffer[129];
    for (const auto& message : messages) {
        const size_t length = std::strlen(message.text);
        for (uint32_t capacity : {1u, static_cast<uint32_t>(length), static_cast<uint32_t>(length + 1), 128u}) {
            std::memset(buffer, '#', sizeof(buffer));
            SErrSetLastError(0x12345678);
            errno = EDOM;
            const bool success = SErrGetErrorStr(message.code, buffer, capacity);
            const bool fits = capacity > length;
            Check(success == (fits || message.code == 108), "known message capacity contract");
            const std::string expected = fits ? message.text : message.code == 108 ? std::string(message.text).substr(0, capacity - 1) : "";
            Check(buffer == expected, "exact or truncated message text");
            Check(buffer[capacity] == '#', "lookup stays within capacity");
            Check(SErrGetLastError() == 0x12345678 && errno == EDOM, "successful/short lookup preserves separate error domains");
        }
    }
    for (uint32_t code : {0u, 12u, 22u, 0xdeadbeefu, 0x80070002u, static_cast<uint32_t>(STORM_ERROR_BAD_ARGUMENT), static_cast<uint32_t>(STORM_ERROR_NOT_ENOUGH_ARGUMENTS)}) {
        buffer[0] = '#';
        SErrSetLastError(71);
        errno = ENOMEM;
        Check(!SErrGetErrorStr(code, buffer, sizeof(buffer)) && !buffer[0], "unsupported codes fail with empty output, not strerror");
        Check(SErrGetLastError() == 71 && errno == ENOMEM, "unknown lookup preserves error domains");
    }
    Check(!SErrGetErrorStr(108, nullptr, 10) && SErrGetLastError() == STORM_ERROR_INVALID_PARAMETER, "null buffer rejected");
    buffer[0] = '#';
    Check(!SErrGetErrorStr(108, buffer, 0) && buffer[0] == '#' && SErrGetLastError() == STORM_ERROR_INVALID_PARAMETER, "zero capacity rejected without write");
    SErrSetLastError(17);
    std::thread separate([] { SErrSetLastError(23); char text[64]; SErrGetErrorStr(108, text, sizeof(text)); Check(SErrGetLastError() == 23, "worker lookup preserves worker TLS"); });
    separate.join();
    Check(SErrGetLastError() == 17, "last error remains thread local");
}
int32_t STORMAPI Continue(uint32_t error, const char*, const char* file, int32_t line, const char* text) {
    std::fprintf(stderr, "HANDLER code=%u source=%s:%d length=%zu\n", error, file, line, std::strlen(text));
    return 0;
}
int32_t STORMAPI Recursive(uint32_t, const char*, const char*, int32_t, const char*) {
    SErrDisplayAppFatal("recursive fatal");
    return 0;
}
int32_t STORMAPI Formatted(uint32_t error, const char*, const char* file, int32_t line, const char* text) {
    Check(error == STORM_ERROR_ASSERTION && !std::strcmp(file, "format.cpp") && line == 42, "formatted report context");
    Check(std::strlen(text) == 2047 && text[2046] == 'L', "formatted report bounded to 2047 characters");
    return 0;
}
void CustomV(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(STORM_ERROR_ASSERTION, format, args);
    va_end(args);
}
int Child(const std::string& mode) {
    SErrRegisterHandler(mode == "recursive" ? Recursive : Continue);
    if (mode == "plain") SErrDisplayAppFatal("fatal value %d", 42);
    if (mode == "prepared") {
        SErrPrepareAppFatal("first.cpp", 1);
        SErrPrepareAppFatal("prepared.cpp", 73);
        SErrDisplayAppFatalCustom(STORM_ERROR_ASSERTION, "custom %s", "text");
    }
    if (mode == "isolated") {
        SErrPrepareAppFatal("must-not-leak.cpp", 91);
        std::thread other([] { SErrDisplayAppFatal("other thread"); });
        other.join();
    }
    if (mode == "owner") {
        SErrPrepareAppFatal("owner.cpp", 81);
        std::thread other([] { SErrPrepareAppFatal("other.cpp", 82); });
        other.join();
        SErrDisplayAppFatal("owner thread");
    }
    if (mode == "message") SErrDisplayAppFatalMessage("literal %s message", "message.cpp", 19);
    if (mode == "long") CustomV("%s", std::string(10000, 'L').c_str());
    if (mode == "recursive") {
        SErrPrepareAppFatal("recursive.cpp", 93);
        SErrDisplayAppFatal("outer fatal");
    }
    return 99;
}
struct Result { int status; std::string output; };
Result Run(const char* executable, const char* mode) {
    int pipefd[2];
    if (pipe(pipefd)) { Check(false, "create stderr pipe"); return {-1, {}}; }
    const pid_t child = fork();
    if (!child) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(98);
        close(pipefd[1]);
        execl(executable, executable, "--child", mode, static_cast<char*>(nullptr));
        _exit(97);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); Check(false, "fork child"); return {-1, {}}; }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    Result result{-1, {}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool done = false;
    while (!done) {
        char buffer[4096];
        ssize_t size;
        while ((size = read(pipefd[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, size);
        const pid_t waited = waitpid(child, &result.status, WNOHANG);
        if (waited == child) done = true;
        else if (waited < 0 || std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &result.status, 0);
            Check(false, "fatal child completes before deadline");
            done = true;
        } else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    char buffer[4096];
    ssize_t size;
    while ((size = read(pipefd[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, size);
    close(pipefd[0]);
    Check(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 1, "fatal exits 1 even with continuing handler");
    Check(result.output.find("ERROR: AddressSanitizer") == std::string::npos &&
          result.output.find("runtime error:") == std::string::npos, "fatal child has no sanitizer findings");
    return result;
}
}
int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--child")) return Child(argv[2]);
    TestMessages();
    SErrRegisterHandler(Formatted);
    Check(SErrDisplayErrorFmt(STORM_ERROR_ASSERTION, "format.cpp", 42, 1, 9, "%s", std::string(10000, 'L').c_str()) == 1, "recoverable formatted report returns handler choice");
    Check(!SErrIsDisplayingError(), "recoverable report clears reporting state");
    SErrDestroy();
    struct Case { const char* mode; const char* expected; };
    for (const auto& test : {Case{"plain", "fatal value 42"}, Case{"prepared", "source=prepared.cpp:73"}, Case{"isolated", "source=:0"}, Case{"owner", "source=owner.cpp:81"}, Case{"message", "literal %s message"}, Case{"long", "length=2047"}, Case{"recursive", "outer fatal"}}) {
        const auto result = Run("/proc/self/exe", test.mode);
        Check(result.output.find(test.expected) != std::string::npos, test.mode);
        if (!std::strcmp(test.mode, "isolated")) Check(result.output.find("must-not-leak") == std::string::npos, "prepared source cannot cross threads");
        if (!std::strcmp(test.mode, "recursive")) Check(result.output.find("recursive fatal") == std::string::npos, "recursive reporting terminates without a second report");
        if (!std::strcmp(test.mode, "prepared")) Check(result.output.find("HANDLER code=2232418304") != std::string::npos, "custom error code preserved");
        if (!std::strcmp(test.mode, "message")) Check(result.output.find("source=message.cpp:19") != std::string::npos, "literal message source preserved");
    }
    if (!failures) std::puts("PASS: Storm message contracts, errno/TLS isolation, bounded formatting and fatal exec reporting");
    return failures ? 1 : 0;
}
