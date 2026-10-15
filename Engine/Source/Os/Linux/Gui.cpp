#include <cstdio>
#include <SDL.h>
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/Input.h"

namespace {
SDL_Window* s_window;
bool s_resizeLock;
}

void* OsGuiGetWindow(int32_t type) {
    switch (type) {
        case 0: return s_window;
        case 1:
        case 2: return SDL_GetKeyboardFocus();
        default: return nullptr;
    }
}

void OsGuiSetGxWindow(void* window) {
    s_window = static_cast<SDL_Window*>(window);
    if (s_window) SDL_SetWindowResizable(s_window, s_resizeLock ? SDL_FALSE : SDL_TRUE);
}

void OsInputSetWindowResizeLock(int32_t resizeLock) {
    s_resizeLock = resizeLock != 0;
    if (s_window) SDL_SetWindowResizable(s_window, s_resizeLock ? SDL_FALSE : SDL_TRUE);
}

void OsGuiSetWindowTitle(void* window, const char* title) {
    SDL_SetWindowTitle(static_cast<SDL_Window*>(window), title);
}

int32_t OsGuiIsModifierKeyDown(int32_t key) {
    const SDL_Keymod modifiers = SDL_GetModState();
    switch (key) {
        case 0: return (modifiers & KMOD_CTRL) != 0;
        case 1: return (modifiers & KMOD_SHIFT) != 0;
        case 2: return (modifiers & KMOD_ALT) != 0;
        default: return 0;
    }
}

int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title) {
    const SDL_MessageBoxButtonData ok[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"},
    };
    const SDL_MessageBoxButtonData okCancel[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    const SDL_MessageBoxButtonData yesNo[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Yes"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "No"},
    };
    const SDL_MessageBoxButtonData yesNoCancel[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Yes"}, {0, 1, "No"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = static_cast<SDL_Window*>(parentWindow);
    box.title = title ? title : "";
    box.message = message ? message : "";
    switch (style) {
        case 1: box.buttons = okCancel; box.numbuttons = 2; break;
        case 2: box.buttons = yesNo; box.numbuttons = 2; break;
        case 3: box.buttons = yesNoCancel; box.numbuttons = 3; break;
        default: box.buttons = ok; box.numbuttons = 1; break;
    }
    int button = 2;
    if (SDL_ShowMessageBox(&box, &button) != 0) {
        std::fprintf(stderr, "OsGuiMessageBox: %s\n", SDL_GetError());
        return 2;
    }
    return button < 0 ? 2 : button;
}

void OsGuiBeep() {
    std::fputc('\a', stderr);
}
