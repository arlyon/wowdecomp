#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <SDL.h>
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Window.h"

namespace {
int failures;
unsigned polls;
uint32_t windowId;
SDL_Window* resizeWindow;
SDL_bool resizeValue;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void Push(SDL_Event event) {
    Check(SDL_PushEvent(&event) == 1, "push SDL event");
}

OSEVENT Next() {
    OSEVENT event{};
    int32_t available = OsInputGet(&event.id, &event.param[0], &event.param[1], &event.param[2], &event.param[3]);
    if (!available) {
        // SDL2 can end the previous poll cycle at its sentinel before reading
        // a synthetic event appended afterward. Model the next main-loop poll.
        available = OsInputGet(&event.id, &event.param[0], &event.param[1], &event.param[2], &event.param[3]);
    }
    Check(available != 0, "expected translated input event within two poll cycles");
    return event;
}

SDL_Event Key(uint32_t type, SDL_Scancode scan) {
    SDL_Event event{};
    event.type = type;
    event.key.windowID = windowId;
    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = scan;
    return event;
}

SDL_Event Window(uint8_t type) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = windowId;
    event.window.event = type;
    return event;
}

void CheckKeys() {
    auto down = Key(SDL_KEYDOWN, SDL_SCANCODE_A);
    down.key.keysym.sym = 0x0444; // A physical key with a non-Latin layout.
    Push(down);
    auto event = Next();
    Check(event.id == OS_INPUT_KEY_DOWN && event.param[0] == KEY_A && event.param[1] == 1,
          "physical key and original repeat-count contract");
    Check(OsInputIsKeyDown(KEY_A) != 0, "key-down state");
    down.key.repeat = 1;
    Push(down);
    event = Next();
    Check(event.id == OS_INPUT_KEY_DOWN && event.param[1] == 1, "repeated key preserves repeat count");
    Push(Key(SDL_KEYUP, SDL_SCANCODE_A));
    event = Next();
    Check(event.id == OS_INPUT_KEY_UP && !OsInputIsKeyDown(KEY_A), "key-up state");
    Push(Key(SDL_KEYDOWN, SDL_SCANCODE_RCTRL));
    event = Next();
    Check(event.param[0] == KEY_RCONTROL && OsInputIsKeyDown(KEY_RCONTROL), "right modifier mapping");
    Push(Window(SDL_WINDOWEVENT_FOCUS_LOST));
    event = Next();
    Check(event.id == OS_INPUT_FOCUS && event.param[0] == 0 && !OsInputIsKeyDown(KEY_RCONTROL),
          "focus loss clears held keys");
    Check(!OsInputIsKeyDown(KEY_NONE), "invalid key sentinel is safe");
    Push(Window(SDL_WINDOWEVENT_FOCUS_GAINED));
    event = Next();
    Check(event.id == OS_INPUT_FOCUS && event.param[0] == 1, "focus gained");
}

void CheckText() {
    SDL_Event text{};
    text.type = SDL_TEXTINPUT;
    text.text.windowID = windowId;
    std::memset(text.text.text, 'a', 31);
    text.text.text[31] = '\0';
    // sdl2-compat's Event2to3 explicitly rejects synthetic text input and can
    // pass nullptr to SDL3_PushEvent. Test the real translator API directly.
    Check(OsGuiProcessMessage(&text) != 0, "accept text commit");
    Push(Key(SDL_KEYDOWN, SDL_SCANCODE_F12));
    for (unsigned i = 0; i < 31; ++i) {
        const auto event = Next();
        Check(event.id == OS_INPUT_CHAR && event.param[0] == 'a' && event.param[1] == 1,
              "31-character commit drains without overflowing the queue or reordering input");
    }
    auto event = Next();
    Check(event.id == OS_INPUT_KEY_DOWN && event.param[0] == KEY_F12, "key follows complete text commit");
    std::strcpy(text.text.text, "A\xC3\xA9\xF0\x9F\x99\x82");
    Check(OsGuiProcessMessage(&text) != 0, "accept Unicode text commit");
    for (int32_t scalar : {0x41, 0xE9, 0x1F642}) {
        event = Next();
        Check(event.id == OS_INPUT_CHAR && event.param[0] == scalar, "UTF-8 commits Unicode scalar values");
    }
}

void CheckMouse() {
    SDL_Event motion{};
    motion.type = SDL_MOUSEMOTION;
    motion.motion.windowID = windowId;
    motion.motion.x = 7;
    motion.motion.y = 11;
    motion.motion.xrel = 2;
    motion.motion.yrel = -3;
    Push(motion);
    auto event = Next();
    Check(event.id == OS_INPUT_MOUSE_MOVE && event.param[0] == 0 &&
          event.param[1] == 7 && event.param[2] == 11, "absolute mouse coordinates and parameter layout");
    SDL_Event button{};
    button.type = SDL_MOUSEBUTTONDOWN;
    button.button.windowID = windowId;
    button.button.button = SDL_BUTTON_LEFT;
    button.button.x = 7;
    button.button.y = 11;
    Push(button);
    event = Next();
    Check(event.id == OS_INPUT_MOUSE_DOWN && event.param[0] == MOUSE_BUTTON_LEFT &&
          OsInputIsMouseButtonDown(MOUSE_BUTTON_LEFT), "mouse button mask and down state");
    Check(!OsInputIsMouseButtonDown(MOUSE_BUTTON_ALL), "unsupported button masks match the Windows API");
    Push(Window(SDL_WINDOWEVENT_FOCUS_LOST));
    event = Next();
    Check(event.id == OS_INPUT_FOCUS && !OsInputIsMouseButtonDown(MOUSE_BUTTON_LEFT),
          "focus loss clears mouse buttons");
    button.type = SDL_MOUSEBUTTONUP;
    Push(button);
    event = Next();
    Check(event.id == OS_INPUT_MOUSE_UP && !OsInputIsMouseButtonDown(MOUSE_BUTTON_LEFT), "mouse up state");
    SDL_Event wheel{};
    wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.windowID = windowId;
    wheel.wheel.y = 2;
    // sdl2-compat's injected SDL3 wheel event does not populate integer_y.
    Check(OsGuiProcessMessage(&wheel) != 0, "accept wheel event");
    event = Next();
    Check(event.id == OS_INPUT_MOUSE_WHEEL && event.param[0] == 240 &&
          event.param[1] == 7 && event.param[2] == 11, "wheel delta uses original 120-unit ticks");
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    Check(OsGuiProcessMessage(&wheel) != 0, "accept flipped wheel event");
    Check(Next().param[0] == -240, "flipped wheel direction");
    OsInputSetMouseSpeed(8.0f);
    Check(OsInputGetMouseSpeed() == 2.0f, "mouse speed upper clamp");
    OsInputSetMouseMode(OS_MOUSE_MODE_RELATIVE);
    Check(SDL_GetRelativeMouseMode() == SDL_TRUE, "enable relative mouse mode");
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    Push(motion);
    event = Next();
    Check(event.id == OS_INPUT_MOUSE_MOVE_RELATIVE && event.param[1] == 4 && event.param[2] == -6,
          "relative motion uses engine mouse speed");
    errno = 0;
    OsInputSetMouseMode(OS_MOUSE_MODES);
    Check(errno == EINVAL && SDL_GetRelativeMouseMode() == SDL_TRUE, "invalid mode leaves relative mode unchanged");
    OsInputSetMouseSpeed(std::numeric_limits<float>::quiet_NaN());
    Check(OsInputGetMouseSpeed() == 2.0f, "nonfinite speed is rejected");
    OsInputSetMouseMode(OS_MOUSE_MODE_NORMAL);
    Check(SDL_GetRelativeMouseMode() == SDL_FALSE, "disable relative mouse mode");
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

void CheckWindows(SDL_Window* window) {
    OsGuiSetWindowTitle(window, "native input test");
    Check(std::strcmp(SDL_GetWindowTitle(window), "native input test") == 0, "window title");
    Check(OsGuiGetWindow(0) == window && OsGuiGetWindow(99) == nullptr, "registered game window");
    OsInputSetWindowResizeLock(1);
    Check(resizeWindow == window && resizeValue == SDL_FALSE, "request resize lock");
    OsInputSetWindowResizeLock(0);
    Check(resizeWindow == window && resizeValue == SDL_TRUE, "request resize unlock");
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    auto size = Window(SDL_WINDOWEVENT_SIZE_CHANGED);
    size.window.data1 = 640;
    size.window.data2 = 480;
    Push(size);
    auto event = Next();
    Check(event.id == OS_INPUT_SIZE && event.param[0] == 640 && event.param[1] == 480, "window resize event");
    OsInputSetEventPollProc([] { ++polls; });
    Push(Window(SDL_WINDOWEVENT_EXPOSED));
    auto foreign = Key(SDL_KEYDOWN, SDL_SCANCODE_Z);
    foreign.key.windowID = windowId + 100;
    Push(foreign);
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    Push(quit);
    event = Next();
    Check(event.id == OS_INPUT_CLOSE && !OsInputIsKeyDown(KEY_Z), "ignore foreign windows and handle quit");
    Check(polls == 1, "exposure invokes registered redraw callback");
    SDL_SetModState(KMOD_CTRL);
    Check(OsGuiIsModifierKeyDown(0) && !OsGuiIsModifierKeyDown(1), "control modifier state");
    SDL_SetModState(KMOD_NONE);
}

void CheckQueue() {
    OsInputInitialize();
    const uint32_t before = OsGetAsyncTimeMs();
    for (int32_t i = 0; i < 20; ++i) OsQueuePut(OS_INPUT_KEY_DOWN, i, 2, 3, -1);
    const uint32_t after = OsGetAsyncTimeMs();
    OSEVENT event{};
    for (int32_t i = 5; i < 20; ++i) {
        Check(OsQueueGet(&event.id, &event.param[0], &event.param[1], &event.param[2], &event.param[3]) &&
              event.param[0] == i && event.param[1] == 2 && event.param[2] == 3,
              "original 15-event drop-oldest queue policy");
        Check(static_cast<uint32_t>(event.param[3]) - before <= after - before, "queue stamps monotonic time");
    }
    Check(!OsQueueGet(&event.id, &event.param[0], &event.param[1], &event.param[2], &event.param[3]), "queue drained");
}

void CheckAsyncState() {
    std::atomic<bool> stop{false};
    std::atomic<unsigned> reads{0};
    std::thread voice([&] {
        while (!stop) {
            OsInputIsKeyDown(KEY_A);
            OsInputIsMouseButtonDown(MOUSE_BUTTON_LEFT);
            ++reads;
        }
    });
    while (!reads) std::this_thread::yield();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    for (unsigned i = 0; i < 32; ++i) {
        Push(Key(SDL_KEYDOWN, SDL_SCANCODE_A));
        Check(Next().id == OS_INPUT_KEY_DOWN, "key down while voice thread polls");
        SDL_Event button{};
        button.type = SDL_MOUSEBUTTONDOWN;
        button.button.windowID = windowId;
        button.button.button = SDL_BUTTON_LEFT;
        button.button.state = SDL_PRESSED;
        Push(button);
        Check(Next().id == OS_INPUT_MOUSE_DOWN, "mouse down while voice thread polls");
        Push(Key(SDL_KEYUP, SDL_SCANCODE_A));
        Check(Next().id == OS_INPUT_KEY_UP, "key up while voice thread polls");
        button.type = SDL_MOUSEBUTTONUP;
        button.button.state = SDL_RELEASED;
        Push(button);
        Check(Next().id == OS_INPUT_MOUSE_UP, "mouse up while voice thread polls");
    }
    stop = true;
    voice.join();
    Check(!OsInputIsKeyDown(KEY_A) && !OsInputIsMouseButtonDown(MOUSE_BUTTON_LEFT), "asynchronous state ends released");
}
}

// The dummy driver cannot change window-manager decorations. Inspect the real
// platform call at its SDK boundary, without a production test hook.
extern "C" void __wrap_SDL_SetWindowResizable(SDL_Window* window, SDL_bool value) {
    resizeWindow = window;
    resizeValue = value;
}

int main() {
    // Never inject into a real desktop or move the user's actual pointer.
    if (SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) || SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL initialization: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("input test", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    windowId = SDL_GetWindowID(window);
    OsInputSetWindowResizeLock(1); // The setting must also survive registration.
    OsGuiSetGxWindow(window);
    Check(resizeWindow == window && resizeValue == SDL_FALSE, "resize lock before window registration");
    OsInputInitialize();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    OSEVENT empty{OS_INPUT_KEY_DOWN, {1, 2, 3, 4}};
    Check(!OsInputGet(&empty.id, &empty.param[0], &empty.param[1], &empty.param[2], &empty.param[3]) &&
          empty.id == OS_INPUT_KEY_DOWN, "empty poll leaves output values valid and unchanged");
    CheckKeys();
    CheckText();
    CheckMouse();
    CheckWindows(window);
    CheckQueue();
    CheckAsyncState();
    OsInputDestroy();
    OsGuiSetGxWindow(nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!failures) std::puts("PASS: SDL input, Unicode, mouse, focus, window and queue contracts (dummy video)");
    return failures ? 1 : 0;
}
