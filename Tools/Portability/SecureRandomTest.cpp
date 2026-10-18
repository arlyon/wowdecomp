#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "Engine/Source/Os/OsSecureRandom.h"

namespace {
enum class Mode { PartialAndInterrupt, Error, Zero };
Mode mode = Mode::PartialAndInterrupt;
unsigned calls = 0;

bool CheckAbort(Mode failure) {
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (!child) {
        const rlimit noCore{0, 0};
        if (setrlimit(RLIMIT_CORE, &noCore)) {
            _exit(2);
        }
        mode = failure;
        std::array<uint8_t, 8> bytes{};
        OsSecureRandom(bytes.data(), bytes.size());
        _exit(3); // A permanent entropy-source failure must never return.
    }
    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
}

// Link this test with --wrap=getrandom: exercise the real implementation at its
// syscall boundary without adding injectable entropy sources to production.
extern "C" ssize_t __wrap_getrandom(void* buffer, size_t size, unsigned int flags) {
    ++calls;
    if (mode == Mode::Error) {
        errno = EIO;
        return -1;
    }
    if (mode == Mode::Zero) {
        return 0;
    }
    if (calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }
    const size_t count = std::min(size, size_t{7});
    std::memset(buffer, 0xA5, count);
    return static_cast<ssize_t>(count);
}

int main() {
    std::array<uint8_t, 33> bytes{};
    IOsSecureRandom(bytes.data() + 1, 31);
    if (calls != 6 || bytes.front() != 0 || bytes.back() != 0 ||
        !std::all_of(bytes.begin() + 1, bytes.end() - 1, [](uint8_t b) { return b == 0xA5; })) {
        std::fputs("FAIL: entropy retry, partial reads or buffer bounds\n", stderr);
        return 1;
    }
    OsSecureRandom(nullptr, 0);
    if (calls != 6) {
        std::fputs("FAIL: zero-size randomness made a syscall\n", stderr);
        return 1;
    }
    if (!CheckAbort(Mode::Error) || !CheckAbort(Mode::Zero)) {
        std::fputs("FAIL: entropy failure did not abort\n", stderr);
        return 1;
    }
    std::puts("PASS: EINTR retry, partial reads, buffer bounds and fail-closed entropy errors");
    return 0;
}
