#include <cctype>
#include <strings.h>
#include "Engine/Source/Storm/H/storm.h"

int32_t STORMAPI SStrCmpI(const char* left, const char* right, size_t count) {
    return strncasecmp(left, right, count);
}

void STORMAPI SStrUpper(char* string) {
    for (; *string; ++string) {
        *string = static_cast<char>(std::toupper(static_cast<unsigned char>(*string)));
    }
}

void STORMAPI SStrLower(char* string) {
    for (; *string; ++string) {
        *string = static_cast<char>(std::tolower(static_cast<unsigned char>(*string)));
    }
}
