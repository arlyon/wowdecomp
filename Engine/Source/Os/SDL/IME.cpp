#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <SDL.h>
#include "Engine/Source/Os/OsIME.h"
#include "Engine/Source/Os/SDL/Window.h"

namespace {
uint32_t s_enableCount;
std::string s_preedit;
int32_t Empty(char* buffer, uint32_t size) {
    if (!buffer || !size) {
        errno = EINVAL;
        return 0;
    }
    std::memset(buffer, 0, size);
    return 1;
}
}

// All entry points run on the SDL video/event owner thread. Never retain an
// SDL_TEXTEDITING_EXT pointer: the input bridge releases it after this call.
void OsIMEUpdatePreedit(const char* text) {
    s_preedit = text ? text : "";
}

void OsIMEInitialize() {
    s_enableCount = 0;
    s_preedit.clear();
    // Ordinary SDL_TEXTINPUT remains enabled by OsInputInitialize even when
    // legacy IME association is disabled (it also supplies non-IME typing).
}

void OsIMEDestroy() {
    s_enableCount = 0;
    s_preedit.clear();
}

void OsIMEEnable(int32_t enable) {
    if (enable) {
        if (s_enableCount != std::numeric_limits<uint32_t>::max()) ++s_enableCount;
        SDL_StartTextInput();
    } else if (s_enableCount && !--s_enableCount) {
        s_preedit.clear();
        // Do not stop ordinary committed text when legacy IME is disabled.
    }
}

void OsIMESync() {
    SDL_StartTextInput();
}

int32_t OsIMEGetCompositionString(char* buffer, uint32_t size) {
    if (!Empty(buffer, size)) return 0;
    if (s_preedit.size() >= size) {
        errno = ERANGE;
        return 0;
    }
    std::memcpy(buffer, s_preedit.c_str(), s_preedit.size() + 1);
    return !s_preedit.empty();
}

int32_t OsIMEGetCompositionResult(char* buffer, uint32_t size) {
    if (Empty(buffer, size)) errno = ENOTSUP;
    // Committed SDL text is delivered exactly once through OS_INPUT_CHAR.
    return 0;
}

int32_t OsIMEGetClauseInfo(uint32_t& start, uint32_t& end, uint32_t& cursor) {
    start = end = cursor = 0;
    // SDL's selection range is not an IMM clause/attribute list.
    errno = ENOTSUP;
    return 0;
}

int32_t OsIMEGetCandidates(uint32_t, uint32_t& pageStart, uint32_t& pageSize,
    uint32_t& count, uint32_t& selection, TSGrowableArray<OsIMECandidate>& candidates) {
    candidates.Clear();
    pageStart = pageSize = count = selection = 0;
    errno = ENOTSUP;
    return 0;
}

int32_t OsIMEGetLanguageMode() {
    errno = ENOTSUP;
    return 0;
}

void OsIMESetLanguageMode(int32_t) {
    // SDL delegates native conversion mode and candidates to the system IME.
    errno = ENOTSUP;
}
