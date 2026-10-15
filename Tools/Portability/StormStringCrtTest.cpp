#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <thread>
#include "Engine/Source/Storm/H/storm.h"

namespace {
unsigned failures;
void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void CheckStrings() {
    Check(SStrCmpI("Shaders\\Vertex\\UI", "sHADERS\\vERTEX\\ui", STORM_MAX_STR) == 0, "case-insensitive renderer path comparison");
    Check(SStrCmpI("prefixA", "PREFIXz", 6) == 0 && SStrCmpI("prefixA", "PREFIXz", 7) < 0,
          "comparison stops at the byte-count boundary");
    Check(SStrCmpI("a", "Z", 1) < 0 && SStrCmpI("Z", "a", 1) > 0, "comparison ordering");
    Check(SStrCmpI("", "", STORM_MAX_STR) == 0 && SStrCmpI("a", "z", 0) == 0, "empty and zero-count comparisons");
    char text[] = "Shaders\\Vertex\\UI.vs 0123_+-";
    SStrUpper(text);
    Check(std::strcmp(text, "SHADERS\\VERTEX\\UI.VS 0123_+-") == 0, "in-place upper-case conversion");
    SStrLower(text);
    Check(std::strcmp(text, "shaders\\vertex\\ui.vs 0123_+-") == 0, "in-place lower-case conversion");
    char empty[] = "";
    SStrUpper(empty);
    SStrLower(empty);
    Check(empty[0] == 0, "empty conversion stays terminated");
    // Exercise high-bit bytes without passing negative char values to ctype.
    std::array<char, 256> bytes{};
    for (unsigned i = 1; i < 256; ++i) bytes[i - 1] = static_cast<char>(i);
    SStrUpper(bytes.data());
    for (unsigned i = 1; i < 256; ++i) {
        Check(static_cast<unsigned char>(bytes[i - 1]) == std::toupper(static_cast<unsigned char>(i)), "upper-case byte domain");
    }
    for (unsigned i = 1; i < 256; ++i) bytes[i - 1] = static_cast<char>(i);
    SStrLower(bytes.data());
    for (unsigned i = 1; i < 256; ++i) {
        Check(static_cast<unsigned char>(bytes[i - 1]) == std::tolower(static_cast<unsigned char>(i)), "lower-case byte domain");
    }
    Check(bytes.back() == 0, "conversion does not overwrite terminator");
}

void CheckErrorState() {
    Check(SErrGetLastError() == 0, "initial error state");
    SErrSetLastError(0xE0001234);
    errno = EACCES;
    Check(SErrGetLastError() == 0xE0001234, "Storm error codes are independent of errno");
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> isolated{true};
    std::array<std::thread, 8> workers;
    for (unsigned i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            if (SErrGetLastError() != 0) isolated = false;
            const uint32_t error = 0xE0000000 | i;
            SErrSetLastError(error);
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (unsigned j = 0; j < 1000; ++j) {
                if (SErrGetLastError() != error) isolated = false;
            }
        });
    }
    while (ready.load() != workers.size()) std::this_thread::yield();
    start = true;
    for (auto& worker : workers) worker.join();
    Check(isolated && SErrGetLastError() == 0xE0001234, "error state is isolated between all threads");
    SErrSetLastError(0);
    Check(SErrGetLastError() == 0, "last error can be cleared explicitly");
}
}

int main() {
    CheckStrings();
    CheckErrorState();
    if (!failures) std::puts("PASS: native Storm string CRT bindings and thread-local error state");
    return failures ? 1 : 0;
}
