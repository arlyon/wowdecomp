#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <sys/wait.h>
#include <unistd.h>
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/SOption.h"

namespace {
unsigned failures;
unsigned reports;
void Check(bool valid, const char* message) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
int32_t STORMAPI Recover(uint32_t error, const char*, const char*, int32_t, const char*) {
    if (error == STORM_ERROR_NOT_ENOUGH_MEMORY) ++reports;
    return 0;
}
}

int main() {
    SMemSetDebugFlags(0, SMEM_DEBUG_HEAPTRACKING);
    auto scalar = new (std::nothrow) uint64_t{42};
    Check(scalar && *scalar == 42, "nothrow scalar allocation");
    delete scalar;
    auto array = new (std::nothrow) uint64_t[5]{};
    Check(array && array[4] == 0, "nothrow array allocation");
    delete[] array;
    struct ConstructorFailure {};
    struct Throwing { Throwing() { throw ConstructorFailure{}; } };
    bool unwound = false;
    try { (void)new (std::nothrow) Throwing; }
    catch (const ConstructorFailure&) { unwound = true; }
    Check(unwound, "nothrow constructor failure invokes matching deallocation");
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        // Recycle a dirty small allocation before growing a smaller one into
        // allocator slack. Growth must not expose bytes from its prior owner.
        void* dirty = SMemAlloc(24, __FILE__, __LINE__, 0);
        std::memset(dirty, 0xA5, 24);
        SMemFree(dirty, __FILE__, __LINE__, 0);
        auto bytes = static_cast<uint8_t*>(SMemAlloc(8, __FILE__, __LINE__, 0));
        std::memset(bytes, 0x77, 8);
        bytes = static_cast<uint8_t*>(SMemReAlloc(bytes, 20, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        for (unsigned i = 0; i < 20; ++i) Check(bytes[i] == (i < 8 ? 0x77 : 0), "zero growth includes allocator slack");
        auto blocked = SMemReAlloc(bytes, 100, __FILE__, __LINE__, SMEM_FLAG_NOREALLOC);
        Check(!blocked && bytes[0] == 0x77, "NOREALLOC leaves original allocation alive");
        Check(!SMemReAlloc(bytes, 100, __FILE__, __LINE__, SMEM_REALLOC_NULL), "REALLOC_NULL sentinel");
        auto shrunk = static_cast<uint8_t*>(SMemReAlloc(bytes, 8, __FILE__, __LINE__, 0));
        bytes = static_cast<uint8_t*>(SMemReAlloc(shrunk, 20, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        for (unsigned i = 8; i < 20; ++i) Check(bytes[i] == 0, "shrink then zero growth clears discarded bytes");
        Check(!SMemReAlloc(bytes, 0, __FILE__, __LINE__, 0), "zero-size realloc frees and returns null");
    }
    auto zero = static_cast<uint8_t*>(SMemAlloc(31, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
    for (unsigned i = 0; i < 31; ++i) Check(zero[i] == 0, "zero allocation");
    SMemFree(zero, __FILE__, __LINE__, 0);
    SErrRegisterHandler(Recover);
    SMemSetDebugFlags(SMEM_DEBUG_NOEXITONFAILURE, SMEM_DEBUG_NOEXITONFAILURE);
    Check(!SMemAlloc(std::numeric_limits<size_t>::max(), __FILE__, __LINE__, 0), "allocation-size overflow is rejected");
    Check(reports == 1 && SErrGetLastError() == STORM_ERROR_NOT_ENOUGH_MEMORY, "recoverable allocation failure reports a real error");
    void* original = SMemAlloc(16, __FILE__, __LINE__, 0);
    std::memset(original, 0x42, 16);
    Check(!SMemReAlloc(original, std::numeric_limits<size_t>::max(), __FILE__, __LINE__, 0) &&
          static_cast<uint8_t*>(original)[0] == 0x42, "failed realloc retains original bytes");
    SMemFree(original, __FILE__, __LINE__, 0);
    SErrDestroy();
    SMemSetDebugFlags(0, SMEM_DEBUG_NOEXITONFAILURE);
    const pid_t child = fork();
    Check(child >= 0, "fatal-error subprocess can start");
    if (!child) SErrDisplayError(STORM_ERROR_FATAL, "test", 1, "expected fatal error", 0, 23, 0);
    if (child > 0) {
        int status = 0;
        Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23,
              "fatal errors terminate with the requested exit status");
    }
    char text[16];
    Check(SStrPrintf(text, sizeof(text), "%s %u", "value", 42u) == 8 && std::strcmp(text, "value 42") == 0,
          "real shared string formatting links and runs");
    g_opt.orderedprintfenabled = 1;
    Check(SStrPrintf(text, sizeof(text), "%2$s %1$d", 42, "value") == 8 && std::strcmp(text, "value 42") == 0,
          "shared string API dispatches to real ordered formatting");
    g_opt.orderedprintfenabled = 0;
    char* duplicate = SStrDupA("shader path", __FILE__, __LINE__);
    Check(duplicate && std::strcmp(duplicate, "shader path") == 0, "shared string duplication uses real allocator");
    SMemFree(duplicate, __FILE__, __LINE__, 0);
    if (!failures) std::puts("PASS: real allocator, zero growth, failure handling, fatal process exit and shared string APIs");
    return failures ? 1 : 0;
}
