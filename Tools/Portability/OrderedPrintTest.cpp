#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
#include "Engine/Source/Storm/H/storm.h"

namespace {
unsigned failures;
int Format(char* output, size_t capacity, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int count = vsnoprintf(output, capacity, format, args);
    va_end(args);
    return count;
}
void Check(bool valid, const char* text) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
}

int main() {
    char buffer[128];
    int count = Format(buffer, sizeof(buffer), "%2$s:%1$d:%3$.2f:%%", 42, "value", 1.25);
    Check(count == static_cast<int>(std::strlen("value:42:1.25:%")) && std::strcmp(buffer, "value:42:1.25:%") == 0, "mixed ordered arguments and percent escape");
    Format(buffer, sizeof(buffer), "%1$I64d", std::numeric_limits<int64_t>::min());
    Check(std::strcmp(buffer, "-9223372036854775808") == 0, "Microsoft signed 64-bit format");
    Format(buffer, sizeof(buffer), "%1$llx", static_cast<int64_t>(-1));
    Check(std::strcmp(buffer, "ffffffffffffffff") == 0, "native long-long hexadecimal format");
    Format(buffer, sizeof(buffer), "%1$I64u", static_cast<int64_t>(-1));
    Check(std::strcmp(buffer, "18446744073709551615") == 0, "unsigned 64-bit format");
    for (size_t capacity = 0; capacity <= 9; ++capacity) {
        char output[16];
        std::memset(output, '#', sizeof(output));
        count = Format(output, capacity, "%1$s", "abcdefgh");
        const size_t expected = capacity ? (capacity - 1 < 8 ? capacity - 1 : 8) : 0;
        Check(count == static_cast<int>(expected), "bounded result is written length");
        Check(output[capacity] == '#', "formatting does not overwrite buffer guard");
        if (capacity) {
            Check(output[expected] == 0 && std::memcmp(output, "abcdefgh", expected) == 0,
                  "truncation preserves every available payload byte and terminator");
        } else Check(output[0] == '#', "zero capacity leaves output untouched");
    }
    for (const char* invalid : {"%65$d", "%999999999999999999999999$d", "%0$d", "%2$d", "%1$d %1$s"}) {
        buffer[0] = '#';
        Check(Format(buffer, sizeof(buffer), invalid, 1) == 0 && buffer[0] == 0,
              "invalid, overflowing, missing or conflicting argument positions are rejected");
    }
    if (!failures) std::puts("PASS: ordered formatting, 64-bit conversions, truncation and invalid argument positions");
    return failures ? 1 : 0;
}
