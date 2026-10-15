#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/CRandom.h"

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
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
    TSBaseArray<int> array{};
    Check(std::strcmp(array.MemFileName(), typeid(int).name()) == 0,
          "native RTTI diagnostic type name");
    CheckNaN();
    CheckRandom();
    if (!failures) {
        std::puts("PASS: compiler helpers, NaN boundaries, and deterministic random vectors/ranges");
    }
    return failures ? 1 : 0;
}
