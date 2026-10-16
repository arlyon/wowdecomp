#include <array>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gxu/GxuScene.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/CRandom.h"

static_assert(sizeof(KEY) == 4 && sizeof(MOUSEBUTTON) == 4 && sizeof(SCHEDSTATE) == 4,
              "input enum widths changed");
static_assert(KEY_NONE == UINT32_MAX && MOUSE_BUTTON_ALL == UINT32_MAX &&
              _UNIQUE_SYMBOL_SCHEDSTATE_96 == UINT32_MAX, "input sentinel values changed");

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool callbackCalled;

void STORM_FASTCALL Callback() {
    callbackCalled = true;
}

static_assert(std::is_same<GxDeviceCallback, decltype(&Callback)>::value,
              "renderer callback calling convention changed");

int Format(char* output, size_t capacity, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = STORM_VSNPRINTF(output, capacity, format, args);
    va_end(args);
    return result;
}

void CheckCompilerBoundary() {
    GxDeviceCallback callback = Callback;
    callback();
    Check(callbackCalled, "renderer callback ABI");
    auto memory = static_cast<uint8_t*>(STORM_ALLOCA(17));
    for (unsigned i = 0; i < 17; ++i) memory[i] = static_cast<uint8_t>(i);
    for (unsigned i = 0; i < 17; ++i) Check(memory[i] == i, "native stack allocation");
    char text[8]{};
    Format(text, sizeof(text), "%s %u", "abcdefg", 42u);
    Check(text[7] == '\0' && std::strcmp(text, "abcdefg") == 0, "native bounded log formatting");
    Check(_stricmp("Shaders", "SHADERS") == 0 && _strnicmp("prefixA", "PREFIXz", 6) == 0,
          "shared archive CRT comparison spellings");
    Check(_atoi64(" -9223372036854775808tail") == INT64_MIN && _atoi64("invalid") == 0,
          "shared manifest signed 64-bit parsing");
    Check(_strtoi64("-0x7fffffffffffffff!", nullptr, 0) == -INT64_MAX &&
          _strtoui64("0xffffffffffffffff!", nullptr, 0) == UINT64_MAX,
          "native base-detected signed and unsigned 64-bit parsing");
    errno = 0;
    Check(_strtoi64("9223372036854775808", nullptr, 10) == INT64_MAX && errno == ERANGE,
          "native signed parsing overflow reports range error");
    Check(_finite(0.0) && _finite(-0.0f) && !_finite(std::numeric_limits<double>::infinity()) &&
          !_finite(std::numeric_limits<float>::quiet_NaN()), "native finite classification");
    static_assert(STORM_ENUM32_BIT31 == uint32_t{1} << 31, "native enum high bit");
    FILE* file = nullptr;
    Check(fopen_s(&file, "/dev/null", "r") == 0 && file, "native checked file-open binding");
    if (file) std::fclose(file);
    Check(fopen_s(&file, nullptr, "r") == EINVAL && !file && errno == EINVAL,
          "checked file-open rejects invalid input and clears output");
}

void CheckNaN() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    Check(IsNaN(nan) != 0, "NaN is detected");
    Check(IsNaN(-nan) != 0, "negative NaN is detected");
    for (double value : {0.0, -0.0, 1.0, -1.0, infinity, -infinity,
                         std::numeric_limits<double>::max()}) {
        Check(IsNaN(value) == 0, "finite values and infinities are not NaNs");
    }
}

void CheckRandom() {
    struct Case {
        uint32_t seed;
        std::array<uint32_t, 5> expected;
    };
    // Little-endian reference vectors computed independently from the original
    // table/index/rotate algorithm, not from the implementation under test.
    const Case cases[] = {
        {0, {0x16BE8AA9, 0xBA39C860, 0xB1DFD560, 0x6BBD62DF, 0x900F3784}},
        {1, {0x3E34D164, 0xEC6567FE, 0xDF4C9BCC, 0x2E14327A, 0x765A1BBF}},
        {12345, {0xA04D0406, 0x3098DC7D, 0xA3886C37, 0xECD344D5, 0x9B8992EA}},
        {UINT32_MAX, {0x6C19B47A, 0x7295D588, 0x38340EC2, 0x6831CA7F, 0x541A8A9B}},
    };
    for (const Case& test : cases) {
        CRndSeed seed(test.seed);
        for (uint32_t expected : test.expected) {
            const uint32_t actual = CRandom::uint32_(seed);
            if (actual != expected) {
                std::fprintf(stderr, "seed=%u: expected %08x, got %08x\n",
                             test.seed, expected, actual);
                ++failures;
            }
        }
    }
    CRndSeed seed(12345);
    for (unsigned i = 0; i < 1024; ++i) {
        const float unit = CRandom::ufloat_(seed);
        const float signedUnit = CRandom::sfloat_(seed);
        const float scaled = CRandom::ufloat_(100.0f, seed);
        Check(unit >= 0.0f && unit < 1.0f, "unit random range");
        Check(signedUnit >= -1.0f && signedUnit <= 1.0f, "signed random range");
        Check(scaled >= 0.0f && scaled < 100.0f, "scaled random range/calling convention");
        Check(CRandom::dice_(6, seed) < 6, "dice upper bound");
        Check(CRandom::dice_(0, seed) == 0, "zero-sided dice boundary");
        Check(CRandom::ufloat_(0.0f, seed) == 0.0f, "zero random scale boundary");
    }
}
}

int main() {
    static_assert(std::is_abstract<GxuSceneObject>::value, "native scene object is an interface");
    Check(typeid(GxuSceneObject*).name() != nullptr, "scene interface pointer RTTI links without fake implementations");
    TSBaseArray<int> array{};
    Check(std::strcmp(array.MemFileName(), typeid(int).name()) == 0,
          "native RTTI diagnostic type name");
    CheckCompilerBoundary();
    CheckNaN();
    CheckRandom();
    if (!failures) {
        std::puts("PASS: compiler helpers, NaN boundaries, and deterministic random vectors/ranges");
    }
    return failures ? 1 : 0;
}
