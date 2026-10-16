#include <cerrno>
#include <SDL.h>
#include "Engine/Source/Os/OsClipboard.h"

char* IOsClipboardGetString(void*) {
    SDL_ClearError();
    char* text = SDL_GetClipboardText();
    // SDL may allocate an empty string on failure; distinguish that from a
    // genuinely empty clipboard so the legacy caller can detect failure.
    if (*SDL_GetError()) {
        SDL_free(text);
        return nullptr;
    }
    return text;
}

char* OsClipboardGetString() {
    return IOsClipboardGetString(nullptr);
}

int32_t IOsClipboardPutString(const char* string, void*) {
    if (!string) {
        errno = EINVAL;
        return 0;
    }
    return SDL_SetClipboardText(string) == 0;
}

int32_t OsClipboardPutString(const char* string) {
    return IOsClipboardPutString(string, nullptr);
}

void OsClipboardFreeString(char* string) {
    SDL_free(string);
}
