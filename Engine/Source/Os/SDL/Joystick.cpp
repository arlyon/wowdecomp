#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <SDL.h>
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Joystick.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/SDL/Window.h"

namespace {
struct Stick { int x = -1; int y = -1; };
struct Hat { bool mapped = false; std::array<int, 4> buttons{}; };
struct Device {
    SDL_Joystick* handle = nullptr;
    std::array<Stick, 4> sticks;
    std::array<Hat, 4> hats;
    std::array<bool, 8> sliders{};
    std::array<int, 8> axes{};
    std::array<bool, 32> buttons{};
    std::array<int, 4> directions{};
    bool mouse = true;
    int mouseStick = 0;
    int left = 0;
    int right = 1;
    float speed = 1.5f;
    uint32_t lastTime = 0;
    double dx = 0;
    double dy = 0;
};
// Owner-thread only. Enumeration indices are current SDL indices; once opened,
// an index is bound to that handle/instance, never silently retargeted on hotplug.
// A detached slot stays invalid until explicitly closed and reopened.
std::map<uint32_t, Device> s_devices;

Device* Find(uint32_t index) {
    auto it = s_devices.find(index);
    return it == s_devices.end() ? nullptr : &it->second;
}
bool Attached(const Device* d) {
    return d && d->handle && SDL_JoystickGetAttached(d->handle);
}
bool Axis(const Device* d, int axis) {
    return Attached(d) && axis >= 0 && axis < 8 && axis < SDL_JoystickNumAxes(d->handle);
}
int ButtonLimit(const Device& d) {
    return d.handle ? std::min(32, SDL_JoystickNumButtons(d.handle)) : 0;
}
bool MappedAxis(const Device& d, int axis) {
    for (auto stick : d.sticks) if (stick.x == axis || stick.y == axis) return true;
    return false;
}
int Scale(float value) {
    if (std::abs(value) <= 0.15f) return 0;
    return static_cast<int>((value > 0 ? value - 0.15f : value + 0.15f) / 0.85f * 32767);
}
int Direction(unsigned hat) {
    switch (hat) {
        case SDL_HAT_UP: return 1;
        case SDL_HAT_RIGHTUP: return 2;
        case SDL_HAT_RIGHT: return 3;
        case SDL_HAT_RIGHTDOWN: return 4;
        case SDL_HAT_DOWN: return 5;
        case SDL_HAT_LEFTDOWN: return 6;
        case SDL_HAT_LEFT: return 7;
        case SDL_HAT_LEFTUP: return 8;
        default: return 0;
    }
}
void Poll(Device& d, bool active) {
    active = active && Attached(&d);
    std::array<int, 8> axes{};
    std::array<bool, 32> buttons{};
    std::array<int, 4> directions{};
    std::array<bool, 32> hatButtons{};
    std::array<float, 8> raw{};
    const int axisCount = active ? std::min(8, SDL_JoystickNumAxes(d.handle)) : 0;
    for (int i = 0; i < axisCount; ++i) {
        const int value = SDL_JoystickGetAxis(d.handle, i);
        raw[i] = value < 0 ? value / 32768.0f : value / 32767.0f;
        axes[i] = Scale(d.sliders[i] ? (raw[i] + 1) * 0.5f : raw[i]);
    }
    for (auto stick : d.sticks) {
        if (stick.x < 0 || stick.y < 0) continue;
        const float x = raw[stick.x], y = raw[stick.y];
        const float length = std::sqrt(x * x + y * y);
        const float scale = length > 0.15f ? (length - 0.15f) / (length * 0.85f) : 0;
        axes[stick.x] = static_cast<int>(x * scale * 32767);
        axes[stick.y] = static_cast<int>(y * scale * 32767);
    }
    if (active) {
        for (int i = 0; i < ButtonLimit(d); ++i) buttons[i] = SDL_JoystickGetButton(d.handle, i) != 0;
    }
    for (int i = 0; i < 4; ++i) {
        if (d.hats[i].mapped) {
            unsigned bits = 0;
            const unsigned masks[] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
            for (int j = 0; j < 4; ++j) {
                const int b = d.hats[i].buttons[j];
                hatButtons[b] = true;
                if (buttons[b]) bits |= masks[j];
            }
            directions[i] = Direction(bits);
        } else if (active && i < SDL_JoystickNumHats(d.handle)) {
            directions[i] = Direction(SDL_JoystickGetHat(d.handle, i));
        }
    }
    for (int i = 0; i < 4; ++i) {
        auto stick = d.sticks[i];
        if (stick.x < 0 || stick.y < 0 || (d.mouse && i == d.mouseStick)) continue;
        if (axes[stick.x] != d.axes[stick.x] || axes[stick.y] != d.axes[stick.y])
            OsQueuePut(OS_INPUT_14, i, axes[stick.x], axes[stick.y], 0);
    }
    for (int i = 0; i < 8; ++i) {
        if (!MappedAxis(d, i) && axes[i] != d.axes[i]) OsQueuePut(OS_INPUT_15, i, axes[i], 0, 0);
    }
    for (int i = 0; i < 32; ++i) {
        if (hatButtons[i] || buttons[i] == d.buttons[i]) continue;
        if (d.mouse && (i == d.left || i == d.right)) {
            int x, y;
            OsInputGetMousePosition(&x, &y);
            OsQueuePut(buttons[i] ? OS_INPUT_MOUSE_DOWN : OS_INPUT_MOUSE_UP,
                i == d.left ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT, x, y, 0);
        } else {
            OsQueuePut(buttons[i] ? OS_INPUT_16 : OS_INPUT_17, i, 0, 0, 0);
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (directions[i] != d.directions[i]) OsQueuePut(OS_INPUT_18, i, directions[i], 0, 0);
    }
    auto stick = d.sticks[d.mouseStick];
    const uint32_t now = SDL_GetTicks();
    if (active && d.mouse && stick.x >= 0 && stick.y >= 0) {
        if (d.lastTime) {
            int32_t width = 0;
            auto window = static_cast<SDL_Window*>(OsGuiGetWindow(0));
            if (window) SDL_GetWindowSize(window, &width, nullptr);
            const double distance = std::min(now - d.lastTime, 100u) * 0.001 * width * d.speed;
            const double x = axes[stick.x] / 32767.0, y = axes[stick.y] / 32767.0;
            d.dx += std::copysign(x * x * x * x, x) * distance;
            d.dy += std::copysign(y * y * y * y, y) * distance;
            d.dx = std::clamp(d.dx, double(INT32_MIN), double(INT32_MAX));
            d.dy = std::clamp(d.dy, double(INT32_MIN), double(INT32_MAX));
            const int mx = static_cast<int>(d.dx), my = static_cast<int>(d.dy);
            if (mx || my) {
                int px, py;
                OsInputGetMousePosition(&px, &py);
                OsInputSetMousePosition(static_cast<int>(std::clamp<int64_t>(int64_t(px) + mx, INT32_MIN, INT32_MAX)),
                    static_cast<int>(std::clamp<int64_t>(int64_t(py) + my, INT32_MIN, INT32_MAX)));
                d.dx -= mx;
                d.dy -= my;
            }
        }
        d.lastTime = now;
    } else {
        d.lastTime = 0;
        d.dx = d.dy = 0;
    }
    d.axes = axes;
    d.buttons = buttons;
    d.directions = directions;
    if (d.handle && !SDL_JoystickGetAttached(d.handle)) {
        SDL_JoystickClose(d.handle);
        d.handle = nullptr;
    }
}
}

uint32_t OsJoystickCount() {
    if (!(SDL_WasInit(SDL_INIT_JOYSTICK) & SDL_INIT_JOYSTICK) && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) return 0;
    const int count = SDL_NumJoysticks();
    return count > 0 ? static_cast<uint32_t>(count) : 0;
}

uint32_t OsJoystickOpen(uint32_t index) {
    if (auto d = Find(index)) return Attached(d) ? index : UINT32_MAX;
    if (index >= OsJoystickCount()) return UINT32_MAX;
    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(static_cast<int>(index));
    for (const auto& entry : s_devices) {
        if (Attached(&entry.second) && SDL_JoystickInstanceID(entry.second.handle) == instance) return UINT32_MAX;
    }
    SDL_Joystick* handle = SDL_JoystickOpen(static_cast<int>(index));
    if (!handle) return UINT32_MAX;
    auto& d = s_devices[index];
    d.handle = handle;
    if (SDL_JoystickNumAxes(handle) >= 2) d.sticks[0] = {0, 1};
    return index;
}

void OsJoystickClose(uint32_t index) {
    auto d = Find(index);
    if (!d) return;
    Poll(*d, false);
    if (d->handle) SDL_JoystickClose(d->handle);
    s_devices.erase(index);
}

char* OsJoystickGetName(uint32_t index, char* buffer, uint32_t size) {
    auto d = Find(index);
    if (!Attached(d) || !buffer || !size) return nullptr;
    const char* name = SDL_JoystickName(d->handle);
    if (!name) return nullptr;
    const size_t length = std::min(std::strlen(name), size_t(size - 1));
    std::memcpy(buffer, name, length);
    buffer[length] = '\0';
    return buffer;
}

void OsJoystickMapStick(uint32_t index, int32_t stick, int32_t x, int32_t y) {
    auto d = Find(index);
    if (stick >= 0 && stick < 4 && Axis(d, x) && Axis(d, y) && x != y) {
        Poll(*d, false);
        d->sticks[stick] = {x, y};
    }
}
void OsJoystickMapSlider(uint32_t index, int32_t axis) {
    auto d = Find(index);
    if (Axis(d, axis)) {
        Poll(*d, false);
        d->sliders[axis] = true;
    }
}
void OsJoystickMapHat(uint32_t index, int32_t hat, int32_t up, int32_t right, int32_t down, int32_t left) {
    auto d = Find(index);
    if (!Attached(d) || hat < 0 || hat >= 4) return;
    const int limit = ButtonLimit(*d);
    for (int b : {up, right, down, left}) if (b < 0 || b >= limit) return;
    Poll(*d, false);
    d->hats[hat] = {true, {up, right, down, left}};
}
void OsJoystickConfigureMouseMode(uint32_t index, int32_t stick, int32_t left, int32_t right, float speed) {
    auto d = Find(index);
    if (!Attached(d) || stick < 0 || stick >= 4 || d->sticks[stick].x < 0 || d->sticks[stick].y < 0 ||
        left < 0 || left >= ButtonLimit(*d) || right < 0 || right >= ButtonLimit(*d) ||
        !std::isfinite(speed) || speed < 0) return;
    Poll(*d, false);
    d->mouseStick = stick;
    d->left = left;
    d->right = right;
    d->speed = speed;
}
void OsJoystickSetMouseMode(uint32_t index, int32_t enable) {
    auto d = Find(index);
    if (!Attached(d) || d->mouse == !!enable) return;
    Poll(*d, false);
    d->mouse = enable != 0;
}
void OsJoystickDestroy() {
    while (!s_devices.empty()) OsJoystickClose(s_devices.begin()->first);
}

void OsPollJoysticks(int32_t active) {
    // Do not initialize/enumerate controllers merely to poll an empty set.
    if (s_devices.empty()) return;
    SDL_JoystickUpdate();
    for (auto& entry : s_devices) Poll(entry.second, active != 0);
}
