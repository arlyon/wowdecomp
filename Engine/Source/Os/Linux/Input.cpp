#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <SDL.h>
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/Linux/Window.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/TimeManager.h"

namespace {
std::array<OSEVENT, 16> s_queue;
size_t s_head;
size_t s_tail;
// ComSat reads push-to-talk state from its own thread.
std::array<std::atomic<bool>, KEY_LAST> s_keys{};
std::atomic<uint32_t> s_buttons{0};
std::array<uint32_t, SDL_TEXTINPUTEVENT_TEXT_SIZE> s_text;
size_t s_textPosition;
size_t s_textSize;
int32_t s_mouseX;
int32_t s_mouseY;
float s_mouseSpeed = 1.0f;
OS_MOUSE_MODE s_mouseMode = OS_MOUSE_MODE_NORMAL;
void (*s_pollProc)();

KEY ConvertKey(SDL_Scancode scan) {
    // Match Windows virtual-key semantics for physical letters even when the
    // active keyboard layout produces non-Latin text. Text comes separately.
    if (scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_Z) {
        return static_cast<KEY>(KEY_A + scan - SDL_SCANCODE_A);
    }
    if (scan >= SDL_SCANCODE_1 && scan <= SDL_SCANCODE_9) {
        return static_cast<KEY>(KEY_1 + scan - SDL_SCANCODE_1);
    }
    if (scan >= SDL_SCANCODE_F1 && scan <= SDL_SCANCODE_F12) {
        return static_cast<KEY>(KEY_F1 + scan - SDL_SCANCODE_F1);
    }
    if (scan >= SDL_SCANCODE_KP_1 && scan <= SDL_SCANCODE_KP_9) {
        return static_cast<KEY>(KEY_NUMPAD1 + scan - SDL_SCANCODE_KP_1);
    }
    switch (scan) {
        case SDL_SCANCODE_0: return KEY_0;
        case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
        case SDL_SCANCODE_LCTRL: return KEY_LCONTROL;
        case SDL_SCANCODE_RCTRL: return KEY_RCONTROL;
        case SDL_SCANCODE_LALT: return KEY_LALT;
        case SDL_SCANCODE_RALT: return KEY_RALT;
        case SDL_SCANCODE_SPACE: return KEY_SPACE;
        case SDL_SCANCODE_GRAVE: return KEY_TILDE;
        case SDL_SCANCODE_EQUALS: return KEY_PLUS;
        case SDL_SCANCODE_MINUS: return KEY_MINUS;
        case SDL_SCANCODE_LEFTBRACKET: return KEY_BRACKET_OPEN;
        case SDL_SCANCODE_RIGHTBRACKET: return KEY_BRACKET_CLOSE;
        case SDL_SCANCODE_SLASH: return KEY_SLASH;
        case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
        case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case SDL_SCANCODE_COMMA: return KEY_COMMA;
        case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
        case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
        case SDL_SCANCODE_RETURN: return KEY_ENTER;
        case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SDL_SCANCODE_TAB: return KEY_TAB;
        case SDL_SCANCODE_LEFT: return KEY_LEFT;
        case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
        case SDL_SCANCODE_UP: return KEY_UP;
        case SDL_SCANCODE_DOWN: return KEY_DOWN;
        case SDL_SCANCODE_INSERT: return KEY_INSERT;
        case SDL_SCANCODE_DELETE: return KEY_DELETE;
        case SDL_SCANCODE_HOME: return KEY_HOME;
        case SDL_SCANCODE_END: return KEY_END;
        case SDL_SCANCODE_PAGEUP: return KEY_PAGEUP;
        case SDL_SCANCODE_PAGEDOWN: return KEY_PAGEDOWN;
        case SDL_SCANCODE_CAPSLOCK: return KEY_CAPSLOCK;
        case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUMLOCK;
        case SDL_SCANCODE_SCROLLLOCK: return KEY_SCROLLLOCK;
        case SDL_SCANCODE_PAUSE: return KEY_PAUSE;
        case SDL_SCANCODE_PRINTSCREEN: return KEY_PRINTSCREEN;
        case SDL_SCANCODE_KP_0: return KEY_NUMPAD0;
        case SDL_SCANCODE_KP_PLUS: return KEY_NUMPAD_PLUS;
        case SDL_SCANCODE_KP_MINUS: return KEY_NUMPAD_MINUS;
        case SDL_SCANCODE_KP_MULTIPLY: return KEY_NUMPAD_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return KEY_NUMPAD_DIVIDE;
        case SDL_SCANCODE_KP_DECIMAL: return KEY_NUMPAD_DECIMAL;
        case SDL_SCANCODE_KP_EQUALS: return KEY_NUMPAD_EQUALS;
        case SDL_SCANCODE_KP_ENTER: return KEY_ENTER;
        default: return KEY_NONE;
    }
}

MOUSEBUTTON ConvertButton(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return MOUSE_BUTTON_LEFT;
        case SDL_BUTTON_MIDDLE: return MOUSE_BUTTON_MIDDLE;
        case SDL_BUTTON_RIGHT: return MOUSE_BUTTON_RIGHT;
        case SDL_BUTTON_X1: return MOUSE_BUTTON_XBUTTON1;
        case SDL_BUTTON_X2: return MOUSE_BUTTON_XBUTTON2;
        default: return MOUSE_BUTTON_NONE;
    }
}

bool GameWindow(uint32_t id) {
    auto window = static_cast<SDL_Window*>(OsGuiGetWindow(0));
    return window && SDL_GetWindowID(window) == id;
}

void ClearKeyState() {
    for (auto& key : s_keys) key = false;
}

void QueueCharacter() {
    OsQueuePut(OS_INPUT_CHAR, static_cast<int32_t>(s_text[s_textPosition++]), 1, 0, 0);
}
}

void OsQueuePut(OSINPUT id, int32_t param0, int32_t param1, int32_t param2, int32_t) {
    // Preserve the original 15-event capacity and drop-oldest overflow policy.
    const size_t next = (s_tail + 1) % s_queue.size();
    if (next == s_head) {
        s_head = (s_head + 1) % s_queue.size();
    }
    s_queue[s_tail] = {id, {param0, param1, param2, static_cast<int32_t>(OsGetAsyncTimeMs())}};
    s_tail = next;
}

int32_t OsQueueGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    if (s_head == s_tail) {
        return 0;
    }
    const OSEVENT& event = s_queue[s_head];
    *id = event.id;
    *param0 = event.param[0];
    *param1 = event.param[1];
    *param2 = event.param[2];
    *param3 = event.param[3];
    s_head = (s_head + 1) % s_queue.size();
    return 1;
}

int32_t OsGuiProcessMessage(void* message) {
    if (!message) {
        return 0;
    }
    const SDL_Event& event = *static_cast<const SDL_Event*>(message);
    switch (event.type) {
        case SDL_QUIT:
            OsQueuePut(OS_INPUT_CLOSE, 0, 0, 0, 0);
            return 1;
        case SDL_WINDOWEVENT:
            if (!GameWindow(event.window.windowID)) return 0;
            OsGuiDispatchNativeWindowEvent(event.window);
            switch (event.window.event) {
                case SDL_WINDOWEVENT_CLOSE:
                    OsQueuePut(OS_INPUT_CLOSE, 0, 0, 0, 0);
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    ClearKeyState();
                    s_buttons = 0;
                    OsQueuePut(OS_INPUT_FOCUS, 0, 0, 0, 0);
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    OsQueuePut(OS_INPUT_FOCUS, 1, 0, 0, 0);
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    OsQueuePut(OS_INPUT_SIZE, event.window.data1, event.window.data2, 0, 0);
                    break;
                case SDL_WINDOWEVENT_EXPOSED:
                    if (s_pollProc) s_pollProc();
                    break;
                default: return 0;
            }
            return 1;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (!GameWindow(event.key.windowID)) return 0;
            const KEY key = ConvertKey(event.key.keysym.scancode);
            if (key == KEY_NONE) return 0;
            const bool down = event.type == SDL_KEYDOWN;
            s_keys[key] = down;
            OsQueuePut(down ? OS_INPUT_KEY_DOWN : OS_INPUT_KEY_UP, key, 1, 0, 0);
            return 1;
        }
        // Preedit UI is separate from committed text. These events transfer
        // SDL-owned allocations even when the game does not consume them.
        case SDL_TEXTEDITING_EXT:
            SDL_free(event.editExt.text);
            return 1;
        case SDL_DROPFILE:
        case SDL_DROPTEXT:
            SDL_free(event.drop.file);
            return 1;
        case SDL_TEXTINPUT: {
            if (!GameWindow(event.text.windowID)) return 0;
            std::array<char, SDL_TEXTINPUTEVENT_TEXT_SIZE> text;
            std::memcpy(text.data(), event.text.text, text.size());
            text.back() = '\0';
            // SDL's UCS-4 convenience macro uses big endian on this host. Select
            // host byte order explicitly so the engine receives scalar values.
            const char* encoding = SDL_BYTEORDER == SDL_LIL_ENDIAN ? "UTF-32LE" : "UTF-32BE";
            auto codepoints = reinterpret_cast<uint32_t*>(
                SDL_iconv_string(encoding, "UTF-8", text.data(), std::strlen(text.data()) + 1));
            if (!codepoints) return 0;
            s_textPosition = 0;
            s_textSize = 0;
            while (s_textSize < s_text.size() && codepoints[s_textSize]) {
                s_text[s_textSize] = codepoints[s_textSize];
                ++s_textSize;
            }
            SDL_free(codepoints);
            // Drain one scalar at a time; a 31-byte commit must not overflow
            // the original 15-event queue and silently lose characters.
            return 1;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (!GameWindow(event.button.windowID)) return 0;
            const MOUSEBUTTON button = ConvertButton(event.button.button);
            if (button == MOUSE_BUTTON_NONE) return 0;
            const bool down = event.type == SDL_MOUSEBUTTONDOWN;
            if (down) s_buttons |= button;
            else s_buttons &= ~static_cast<uint32_t>(button);
            s_mouseX = event.button.x;
            s_mouseY = event.button.y;
            OsQueuePut(down ? OS_INPUT_MOUSE_DOWN : OS_INPUT_MOUSE_UP, button, s_mouseX, s_mouseY, 0);
            return 1;
        }
        case SDL_MOUSEMOTION:
            if (!GameWindow(event.motion.windowID)) return 0;
            if (s_mouseMode == OS_MOUSE_MODE_RELATIVE) {
                const auto scaled = [](int32_t value) {
                    return static_cast<int32_t>(std::clamp(value * static_cast<double>(s_mouseSpeed),
                        static_cast<double>(INT32_MIN), static_cast<double>(INT32_MAX)));
                };
                OsQueuePut(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, scaled(event.motion.xrel), scaled(event.motion.yrel), 0);
            } else {
                s_mouseX = event.motion.x;
                s_mouseY = event.motion.y;
                OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, s_mouseX, s_mouseY, 0);
            }
            return 1;
        case SDL_MOUSEWHEEL: {
            if (!GameWindow(event.wheel.windowID)) return 0;
            const int64_t direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            const int64_t wheel = std::clamp<int64_t>(direction * event.wheel.y * 120, INT32_MIN, INT32_MAX);
            OsQueuePut(OS_INPUT_MOUSE_WHEEL, static_cast<int32_t>(wheel), s_mouseX, s_mouseY, 0);
            return 1;
        }
        default: return 0;
    }
}

int32_t OsInputGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    if (OsQueueGet(id, param0, param1, param2, param3)) return 1;
    if (s_textPosition < s_textSize) {
        QueueCharacter();
        return OsQueueGet(id, param0, param1, param2, param3);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        OsGuiProcessMessage(&event);
        if (s_textPosition < s_textSize) QueueCharacter();
        if (OsQueueGet(id, param0, param1, param2, param3)) return 1;
    }
    return 0;
}

int32_t OsInputIsKeyDown(KEY key) {
    return static_cast<uint32_t>(key) < s_keys.size() && s_keys[key];
}

int32_t OsInputIsMouseButtonDown(MOUSEBUTTON button) {
    switch (button) {
        case MOUSE_BUTTON_LEFT:
        case MOUSE_BUTTON_MIDDLE:
        case MOUSE_BUTTON_RIGHT:
        case MOUSE_BUTTON_XBUTTON1:
        case MOUSE_BUTTON_XBUTTON2:
            return (s_buttons & button) != 0;
        default: return 0;
    }
}

void OsInputSetEventPollProc(void (*pollProc)()) {
    s_pollProc = pollProc;
}

float OsInputGetMouseSpeed() {
    return s_mouseSpeed;
}

void OsInputSetMouseSpeed(float speed) {
    if (!std::isfinite(speed)) {
        errno = EINVAL;
        return;
    }
    s_mouseSpeed = static_cast<int32_t>(std::clamp(speed, 0.1f, 2.0f) * 10.0f) * 0.1f;
}

void OsInputSetMouseMode(OS_MOUSE_MODE mode) {
    if (mode != OS_MOUSE_MODE_NORMAL && mode != OS_MOUSE_MODE_RELATIVE) {
        errno = EINVAL;
        return;
    }
    if (SDL_SetRelativeMouseMode(mode == OS_MOUSE_MODE_RELATIVE ? SDL_TRUE : SDL_FALSE) == 0) {
        s_mouseMode = mode;
    }
}

void OsInputGetMousePosition(int32_t* x, int32_t* y) {
    if (x) *x = s_mouseX;
    if (y) *y = s_mouseY;
}

void OsInputSetMousePosition(int32_t x, int32_t y) {
    s_mouseX = x;
    s_mouseY = y;
    if (s_mouseMode == OS_MOUSE_MODE_NORMAL) {
        if (auto window = static_cast<SDL_Window*>(OsGuiGetWindow(0))) SDL_WarpMouseInWindow(window, x, y);
    }
}

void OsInputInitialize() {
    s_head = s_tail = 0;
    ClearKeyState();
    s_buttons = 0;
    s_textPosition = s_textSize = 0;
    s_mouseSpeed = 1.0f;
    s_mouseMode = OS_MOUSE_MODE_NORMAL;
    SDL_GetMouseState(&s_mouseX, &s_mouseY);
    SDL_StartTextInput();
}

void OsInputDestroy() {
    SDL_StopTextInput();
    SDL_SetRelativeMouseMode(SDL_FALSE);
    s_head = s_tail = 0;
    ClearKeyState();
    s_buttons = 0;
    s_textPosition = s_textSize = 0;
    s_mouseMode = OS_MOUSE_MODE_NORMAL;
    s_pollProc = nullptr;
}

// This hook is also empty in the reconstructed Windows implementation.
void OsInputEnableSecureMode(int32_t) {
}
