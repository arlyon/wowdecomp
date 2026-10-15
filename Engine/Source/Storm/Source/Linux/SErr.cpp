#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <SDL.h>
#include "Engine/Source/Storm/H/storm.h"

namespace {
// Preserve the client error-code domain independently of libc errno. A later
// successful syscall must not erase the last error reported by a Storm API.
thread_local uint32_t s_lastError;
thread_local bool s_reporting;
using ErrorHandler = int32_t (STORMAPI*)(uint32_t, const char*, const char*, int32_t, const char*);
struct Handler { ErrorHandler callback; Handler* next; };
Handler* s_handlers;
std::recursive_mutex s_handlerMutex;
std::thread::id s_uiThread;
std::atomic<bool> s_initialized{false};
}

uint32_t STORMAPI SErrGetLastError() {
    return s_lastError;
}

void STORMAPI SErrSetLastError(uint32_t error) {
    s_lastError = error;
}

void SErrInitialize() {
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    if (s_initialized.load(std::memory_order_acquire)) return;
    s_uiThread = std::this_thread::get_id();
    s_initialized.store(true, std::memory_order_release);
}

void STORMAPI SErrRegisterHandler(ErrorHandler callback) {
    if (!callback) { SErrSetLastError(STORM_ERROR_INVALID_PARAMETER); return; }
    auto node = static_cast<Handler*>(std::malloc(sizeof(Handler)));
    if (!node) { SErrSetLastError(STORM_ERROR_NOT_ENOUGH_MEMORY); return; }
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    *node = {callback, s_handlers};
    s_handlers = node;
}

int32_t STORMAPI SErrDestroy() {
    if (s_reporting) { SErrSetLastError(STORM_ERROR_INVALID_PARAMETER); return 0; }
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    while (s_handlers) {
        Handler* next = s_handlers->next;
        std::free(s_handlers);
        s_handlers = next;
    }
    s_initialized.store(false, std::memory_order_release);
    return 1;
}

int32_t STORMAPI SErrIsDisplayingError() { return s_reporting; }

int32_t STORMAPI SErrDisplayError(uint32_t error, const char* filename, int32_t line,
        const char* description, int32_t recoverable, uint32_t exitCode, uint32_t) {
    if (s_reporting) std::_Exit(static_cast<int>(exitCode));
    s_reporting = true;
    char summary[2048];
    char errorText[64];
    std::snprintf(errorText, sizeof(errorText), "Storm error %u (0x%08x)", error, error);
    std::snprintf(summary, sizeof(summary), "%s\n%s:%d\n%s", errorText,
        filename ? filename : "", line, description ? description : "");
    std::fprintf(stderr, "%s\n", summary);
    bool handled = false;
    bool canShowDialog = false;
    {
        std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
        canShowDialog = s_initialized.load(std::memory_order_acquire) && std::this_thread::get_id() == s_uiThread;
        for (Handler* node = s_handlers; node; node = node->next) {
            if (!node->callback(error, errorText, filename ? filename : "", line, description ? description : "")) {
                handled = true;
                break;
            }
        }
    }
    int choice = 0; // A headless/unavailable dialog must never imply Continue.
    if (recoverable && handled) choice = 1;
    else if (!handled && canShowDialog && SDL_WasInit(SDL_INIT_VIDEO)) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Quit"},
            {0, 1, "Continue"}
        };
        SDL_MessageBoxData box{};
        box.flags = recoverable ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_ERROR;
        box.title = "World of Warcraft";
        box.message = summary;
        box.numbuttons = recoverable ? 2 : 1;
        box.buttons = buttons;
        if (SDL_ShowMessageBox(&box, &choice) != 0) choice = 0;
    }
    if (recoverable && choice == 1) {
        s_reporting = false;
        return 1;
    }
    std::fflush(stderr);
    std::_Exit(static_cast<int>(exitCode));
}
