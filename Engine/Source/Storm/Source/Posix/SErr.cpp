#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <exception>
#include <unistd.h>
#include <thread>
#include <SDL.h>
#include "Engine/Source/Storm/H/storm.h"

namespace {
// Preserve the client error-code domain independently of libc errno. A later
// successful syscall must not erase the last error reported by a Storm API.
thread_local uint32_t s_lastError;
thread_local bool s_reporting;
struct FatalContext { const char* filename; int32_t line; };
thread_local FatalContext s_fatalContext{};
using ErrorHandler = int32_t (STORMAPI*)(uint32_t, const char*, const char*, int32_t, const char*);
struct Handler { ErrorHandler callback; Handler* next; };
Handler* s_handlers;
std::recursive_mutex s_handlerMutex;
std::thread::id s_uiThread;
std::atomic<bool> s_initialized{false};
char s_logTitle[256] = "World of Warcraft";
int32_t (STORMAPI* s_logCallback)(char*, uint32_t);
std::terminate_handler s_previousTerminate;
bool s_catching;
[[noreturn]] void Terminate() noexcept {
    // No allocation, locks, callbacks, dialogs, or attempted signal recovery.
    static constexpr char text[] = "Storm: unhandled C++ exception or terminate request\n";
    (void)!write(STDERR_FILENO, text, sizeof(text) - 1);
    std::abort();
}
}

uint32_t STORMAPI SErrGetLastError() {
    return s_lastError;
}

void STORMAPI SErrSetLastError(uint32_t error) {
    s_lastError = error;
}

int32_t STORMAPI SErrGetErrorStr(uint32_t error, char* buffer, uint32_t bufferchars) {
    if (!buffer || !bufferchars) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *buffer = 0;
    // Native messages are explicit legacy mappings and the Win64 fallback
    // labels, not errno translations or emulated PE message resources.
    const char* text;
    switch (error) {
        case 108:
            std::snprintf(buffer, bufferchars, "%s", "Invalid or corrupt archive.\n");
            return 1; // The reference deliberately accepts truncation here.
        case STORM_ERROR_FILE_NOT_FOUND: text = "The system cannot find the file specified.\r\n"; break;
        case STORM_ERROR_NOT_ENOUGH_MEMORY: text = "Not enough storage is available to process this command.\r\n"; break;
        case STORM_ERROR_INVALID_PARAMETER: text = "The parameter is incorrect.\r\n"; break;
        case STORM_ERROR_ASSERTION: text = "Assertion Failure"; break;
        case STORM_ERROR_NOT_ARCHIVE: text = "Not Archive"; break;
        case STORM_ERROR_VERSION_MISMATCH: text = "Version Mismatch"; break;
        case STORM_ERROR_MEM_NOT_ALLOCATED: text = "Memory Already Freed"; break;
        case STORM_ERROR_MEM_CORRUPTED: text = "Memory Corrupt"; break;
        case STORM_ERROR_MEM_INVALID: text = "Memory Invalid Block"; break;
        case STORM_ERROR_MEM_NOT_FREED: text = "Memory Never Released"; break;
        case STORM_ERROR_RESOURCES_NOT_RELEASED: text = "Handle Never Released"; break;
        case STORM_ERROR_OUT_OF_BOUNDS: text = "Access Out Of Bounds"; break;
        case STORM_ERROR_FILE_CORRUPTED: text = "File Corrupt"; break;
        case STORM_ERROR_FATAL: text = "Fatal Exception"; break;
        case STORM_ERROR_FATAL_CONDITION: text = "Fatal Condition"; break;
        default: return 0;
    }
    const size_t length = std::strlen(text);
    if (length >= bufferchars) return 0;
    std::memcpy(buffer, text, length + 1);
    return 1;
}

void STORMAPI SErrPrepareAppFatal(const char* filename, int32_t line) {
    s_fatalContext = {filename, line};
}

void SErrDisplayAppFatalCustomV(uint32_t error, const char* format, va_list args) {
    const FatalContext context = s_fatalContext;
    s_fatalContext = {};
    char buffer[2048]{};
    std::vsnprintf(buffer, sizeof(buffer), format ? format : "", args);
    buffer[sizeof(buffer) - 1] = 0;
    SErrDisplayError(error, context.filename, context.line, buffer, 0, 1, 0x11111111);
    std::_Exit(1);
}

void SErrDisplayAppFatalCustom(uint32_t error, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(error, format, args);
    va_end(args);
}

void SErrDisplayAppFatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(STORM_ERROR_FATAL_CONDITION, format, args);
    va_end(args);
}

[[noreturn]] void SErrDisplayAppFatalMessage(const char* message, const char* filename, int32_t line) {
    SErrPrepareAppFatal(filename, line);
    SErrDisplayAppFatal("%s", message ? message : "");
    std::_Exit(1);
}

int32_t SErrDisplayErrorFmt(uint32_t error, const char* filename, int32_t line,
        int32_t recoverable, uint32_t exitCode, const char* format, ...) {
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format ? format : "", args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;
    return SErrDisplayError(error, filename, line, buffer, recoverable, exitCode, 0x11111111);
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

void STORMAPI SErrCatchUnhandledExceptions() {
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    if (!s_catching) {
        s_previousTerminate = std::set_terminate(Terminate);
        s_catching = true;
    }
}

void STORMAPI SErrSetLogTitleString(const char* title) {
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    std::snprintf(s_logTitle, sizeof(s_logTitle), "%s", title ? title : "");
}

void STORMAPI SErrSetLogCallback(int32_t (STORMAPI* callback)(char*, uint32_t)) {
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    s_logCallback = callback;
}

int32_t STORMAPI SErrGetLogLastPath(char* buffer, uint32_t size) {
    if (!buffer || !size) SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
    // Native diagnostics currently go to stderr, not a persistent crash report.
    return 0;
}

int32_t STORMAPI SErrDestroy() {
    if (s_reporting) { SErrSetLastError(STORM_ERROR_INVALID_PARAMETER); return 0; }
    std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
    while (s_handlers) {
        Handler* next = s_handlers->next;
        std::free(s_handlers);
        s_handlers = next;
    }
    s_logCallback = nullptr;
    if (s_catching) {
        if (std::get_terminate() == Terminate) std::set_terminate(s_previousTerminate);
        s_catching = false;
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
    char title[sizeof(s_logTitle)];
    {
        std::lock_guard<std::recursive_mutex> lock(s_handlerMutex);
        std::memcpy(title, s_logTitle, sizeof(title));
        std::fprintf(stderr, "%s\n", title);
        if (s_logCallback) {
            char header[4096]{};
            s_logCallback(header, sizeof(header));
            header[sizeof(header) - 1] = 0;
            std::fputs(header, stderr);
        }
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
        box.title = title;
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
