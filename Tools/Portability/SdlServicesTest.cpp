#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <SDL.h>
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Joystick.h"
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Os/OsIME.h"
#include "Engine/Source/Os/Window.h"

namespace {
int failures;
void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); ++failures; }
}
std::vector<OSEVENT> Drain() {
    std::vector<OSEVENT> result;
    OSEVENT e{};
    while (OsQueueGet(&e.id, &e.param[0], &e.param[1], &e.param[2], &e.param[3])) result.push_back(e);
    return result;
}
bool Has(const std::vector<OSEVENT>& events, OSINPUT id, int p0, int p1) {
    for (const auto& e : events) if (e.id == id && e.param[0] == p0 && e.param[1] == p1) return true;
    return false;
}
void Clipboard() {
    Check(OsClipboardPutString("clipboard \xE2\x98\x83") == 1, "UTF-8 clipboard put");
    char* first = OsClipboardGetString();
    Check(first && !std::strcmp(first, "clipboard \xE2\x98\x83"), "UTF-8 clipboard get");
    Check(OsClipboardPutString("next") == 1, "clipboard overwrite");
    Check(first && !std::strcmp(first, "clipboard \xE2\x98\x83"), "clipboard allocation independent");
    OsClipboardFreeString(first);
    OsClipboardFreeString(nullptr);
    Check(!OsClipboardPutString(nullptr) && errno == EINVAL, "null clipboard input");
    Check(OsClipboardPutString("") == 1, "empty clipboard put");
    first = OsClipboardGetString();
    Check(first && !*first, "empty clipboard owned string");
    OsClipboardFreeString(first);
}
void IME(SDL_Window* window) {
    OsIMEInitialize();
    OsIMEEnable(1);
    OsIMEEnable(1);
    SDL_Event event{};
    event.type = SDL_TEXTEDITING;
    event.edit.windowID = SDL_GetWindowID(window);
    std::strcpy(event.edit.text, "preedit \xE2\x98\x83");
    Check(OsGuiProcessMessage(&event) == 1, "preedit bridge");
    char text[128];
    Check(OsIMEGetCompositionString(text, sizeof(text)) > 0 && !std::strcmp(text, event.edit.text), "genuine preedit UTF-8");
    char tiny[2] = {'x', 'x'};
    Check(!OsIMEGetCompositionString(tiny, sizeof(tiny)) && !tiny[0] && errno == ERANGE, "short preedit buffer cleared");
    Check(!OsIMEGetCompositionString(nullptr, 10) && errno == EINVAL, "null composition output");
    event.edit.windowID++;
    event.edit.text[0] = 0;
    Check(!OsGuiProcessMessage(&event), "foreign preedit ignored");
    Check(OsIMEGetCompositionString(text, sizeof(text)) > 0, "foreign event preserves preedit");
    event = {};
    event.type = SDL_TEXTEDITING_EXT;
    event.editExt.windowID = SDL_GetWindowID(window);
    event.editExt.text = SDL_strdup("extended preedit");
    Check(OsGuiProcessMessage(&event) == 1, "extended allocation consumed");
    Check(OsIMEGetCompositionString(text, sizeof(text)) == 1 && !std::strcmp(text, "extended preedit"), "extended preedit copied before free");
    SDL_Event focus{};
    focus.type = SDL_WINDOWEVENT;
    focus.window.windowID = SDL_GetWindowID(window);
    focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    Check(OsGuiProcessMessage(&focus) == 1 && !OsIMEGetCompositionString(text, sizeof(text)) && !text[0], "focus loss clears preedit");
    Drain();
    event.editExt.text = SDL_strdup("extended preedit");
    Check(OsGuiProcessMessage(&event) == 1, "preedit after focus loss");
    OsIMEEnable(0);
    Check(OsIMEGetCompositionString(text, sizeof(text)) > 0, "nested enable retained");
    OsIMEEnable(0);
    OsIMEEnable(0);
    Check(!OsIMEGetCompositionString(text, sizeof(text)) && !*text, "last disable clears preedit");
    OsIMESync();
    Check(SDL_IsTextInputActive(), "ordinary text remains active");
    uint32_t a = 1, b = 2, c = 3, d = 4;
    TSGrowableArray<OsIMECandidate> candidates;
    std::strcpy(candidates.New()->m_text, "stale candidate");
    Check(!OsIMEGetCandidates(1, a, b, c, d, candidates) && !candidates.Count() && !a && !b && !c && !d && errno == ENOTSUP, "candidate unsupported outputs");
    a = b = c = 1;
    Check(!OsIMEGetClauseInfo(a, b, c) && !a && !b && !c && errno == ENOTSUP, "clause unsupported outputs");
    Check(!OsIMEGetLanguageMode() && errno == ENOTSUP, "language query unsupported");
    OsIMESetLanguageMode(1);
    Check(errno == ENOTSUP && !OsIMEGetLanguageMode(), "language set not fabricated");
    std::strcpy(text, "old");
    Check(!OsIMEGetCompositionResult(text, sizeof(text)) && !text[0] && errno == ENOTSUP, "no fabricated composition result");
    event = {};
    event.type = SDL_TEXTINPUT;
    event.text.windowID = SDL_GetWindowID(window);
    std::strcpy(event.text.text, "A\xE2\x98\x83");
    Check(OsGuiProcessMessage(&event) == 1, "committed text accepted");
    OSEVENT e{};
    Check(OsInputGet(&e.id, &e.param[0], &e.param[1], &e.param[2], &e.param[3]) && e.id == OS_INPUT_CHAR && e.param[0] == 'A', "ordinary ASCII commit once");
    Check(OsInputGet(&e.id, &e.param[0], &e.param[1], &e.param[2], &e.param[3]) && e.id == OS_INPUT_CHAR && e.param[0] == 0x2603, "ordinary Unicode commit once");
    Check(!OsIMEGetCompositionString(text, sizeof(text)), "commit clears preedit");
    OsIMEDestroy();
    Check(SDL_IsTextInputActive(), "IME destroy preserves ordinary typing");
}
void JoystickRemapping() {
    Check(OsJoystickCount() == 0, "remapping starts without devices");
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 8, 1);
    Check(index == 0 && OsJoystickOpen(0) == 0, "open virtual remapping fixture");
    if (index < 0) return;
    SDL_Joystick* control = SDL_JoystickOpen(index);
    Check(control != nullptr, "open remapping control handle");
    if (!control) { OsJoystickClose(0); SDL_JoystickDetachVirtual(index); return; }
    OsJoystickSetMouseMode(0, 0);
    Check(SDL_JoystickSetVirtualAxis(control, 0, 32767) == 0 &&
        SDL_JoystickSetVirtualButton(control, 4, 1) == 0, "hold old stick and unmapped button");
    OsPollJoysticks(1);
    auto events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 32767) && Has(events, OS_INPUT_16, 4, 0), "deliver old stick and held button");
    OsJoystickMapStick(0, 9, 2, 3);
    OsJoystickMapHat(0, 0, 4, 5, 6, 99);
    OsJoystickMapSlider(0, -1);
    Check(Drain().empty(), "invalid mappings do not release current state");

    OsJoystickMapHat(0, 0, 4, 5, 6, 7);
    events = Drain();
    Check(Has(events, OS_INPUT_17, 4, 0), "hat remap releases previously delivered held button");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_18, 0, 1) && !Has(events, OS_INPUT_16, 4, 0), "held button now drives hat only");
    OsJoystickMapStick(0, 0, 2, 3);
    events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 0), "stick remap releases nonzero old axes before centered new axes");
    OsPollJoysticks(1);
    events = Drain();
    Check(!Has(events, OS_INPUT_14, 0, 32767) && Has(events, OS_INPUT_15, 0, 32767) &&
        Has(events, OS_INPUT_18, 0, 1), "old axis becomes independent while new stick remains centered");
    OsPollJoysticks(0);
    events = Drain();
    Check(Has(events, OS_INPUT_15, 0, 0) && Has(events, OS_INPUT_18, 0, 0), "focus releases remapped axis and hat");
    OsPollJoysticks(1);
    Drain();
    OsJoystickMapSlider(0, 0);
    events = Drain();
    Check(Has(events, OS_INPUT_15, 0, 0), "slider remap releases previous axis interpretation");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_15, 0, 32767) && Has(events, OS_INPUT_18, 0, 1), "slider and hat state restored after remap");
    Check(SDL_JoystickSetVirtualButton(control, 4, 0) == 0, "release remapped physical button");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_18, 0, 0) && !Has(events, OS_INPUT_17, 4, 0), "physical release ends hat without duplicate button release");
    Check(SDL_JoystickSetVirtualAxis(control, 2, 32767) == 0 &&
        SDL_JoystickSetVirtualButton(control, 4, 1) == 0, "hold new stick and remapped hat for close");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 32767) && Has(events, OS_INPUT_18, 0, 1), "new mapping delivers active stick and hat");
    OsJoystickClose(0);
    events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 0) && Has(events, OS_INPUT_15, 0, 0) &&
        Has(events, OS_INPUT_18, 0, 0), "close releases new stick slider and hat");
    SDL_JoystickClose(control);
    Check(SDL_JoystickDetachVirtual(index) == 0 && OsJoystickCount() == 0, "remapping fixture cleanup");
}
void Joysticks() {
    Check(OsJoystickCount() == 0, "isolated no devices");
    Check(OsJoystickOpen(0) == UINT32_MAX, "missing device open failure");
    const int first = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 8, 1);
    const int second = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 8, 1);
    Check(first == 0 && second == 1 && OsJoystickCount() == 2, "two virtual devices attached");
    if (first < 0 || second < 0) return;
    SDL_Joystick* control = SDL_JoystickOpen(second);
    Check(control != nullptr && OsJoystickOpen(1) == 1, "open virtual second");
    if (!control) return;
    char name[64];
    Check(OsJoystickGetName(1, name, sizeof(name)) && !std::strcmp(name, SDL_JoystickName(control)), "bound name");
    char small[1] = {'x'};
    Check(OsJoystickGetName(1, small, 1) && !small[0], "name truncation");
    Check(!OsJoystickGetName(1, nullptr, 1) && !OsJoystickGetName(1, name, 0), "name invalid buffers");
    Check(SDL_JoystickDetachVirtual(first) == 0 && OsJoystickCount() == 1, "unrelated detach shifts enumeration");
    Check(OsJoystickOpen(1) == 1 && OsJoystickGetName(1, name, sizeof(name)) && !std::strcmp(name, SDL_JoystickName(control)), "open slot not retargeted on shift");
    Check(OsJoystickOpen(0) == UINT32_MAX, "shifted enumeration cannot duplicate bound instance");
    OsJoystickSetMouseMode(1, 0);
    OsJoystickMapStick(1, 1, 2, 3);
    OsJoystickMapStick(1, -1, -1, 99);
    OsJoystickMapHat(1, 0, 4, 5, 6, 7);
    OsJoystickMapHat(1, 9, 100, -1, 0, 0);
    OsPollJoysticks(1);
    Drain();
    Check(SDL_JoystickSetVirtualAxis(control, 0, 32767) == 0 && SDL_JoystickSetVirtualButton(control, 2, 1) == 0 &&
        SDL_JoystickSetVirtualButton(control, 4, 1) == 0 && SDL_JoystickSetVirtualButton(control, 5, 1) == 0, "set virtual state");
    OsPollJoysticks(1);
    auto events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 32767), "mapped stick output");
    Check(Has(events, OS_INPUT_16, 2, 0), "joystick button down");
    Check(Has(events, OS_INPUT_18, 0, 2) && !Has(events, OS_INPUT_16, 4, 0), "button hat diagonal consumes buttons");
    OsPollJoysticks(0);
    events = Drain();
    Check(Has(events, OS_INPUT_14, 0, 0) && Has(events, OS_INPUT_17, 2, 0) && Has(events, OS_INPUT_18, 0, 0), "focus loss releases all state");
    OsPollJoysticks(0);
    Check(Drain().empty(), "inactive no repeated input");
    OsJoystickConfigureMouseMode(1, 0, 0, 1, 1.0f);
    OsJoystickSetMouseMode(1, 1);
    Check(SDL_JoystickSetVirtualButton(control, 0, 1) == 0, "virtual mouse button");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT, 0), "mouse mode button translation");
    int mouseX, mouseY;
    OsInputGetMousePosition(&mouseX, &mouseY);
    Check(SDL_JoystickDetachVirtual(0) == 0, "detach bound device");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, mouseX), "detach releases mouse state");
    Check(!OsJoystickGetName(1, name, sizeof(name)) && OsJoystickOpen(1) == UINT32_MAX, "detached slot invalid");
    SDL_JoystickClose(control);
    OsJoystickClose(1);
    OsJoystickClose(1);
    Check(SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 8, 1) == 0 && OsJoystickOpen(0) == 0, "explicit new open");
    control = SDL_JoystickOpen(0);
    OsJoystickSetMouseMode(0, 0);
    OsJoystickMapSlider(0, 2);
    Check(SDL_JoystickSetVirtualAxis(control, 2, 32767) == 0 && SDL_JoystickSetVirtualHat(control, 0, SDL_HAT_LEFTDOWN) == 0, "set slider and native hat");
    OsPollJoysticks(1);
    events = Drain();
    Check(Has(events, OS_INPUT_15, 2, 32767) && Has(events, OS_INPUT_18, 0, 6), "slider and native hat output");
    OsJoystickClose(0);
    events = Drain();
    Check(Has(events, OS_INPUT_15, 2, 0) && Has(events, OS_INPUT_18, 0, 0), "close releases slider and hat");
    SDL_JoystickClose(control);
    Check(SDL_JoystickDetachVirtual(0) == 0 && OsJoystickCount() == 0, "virtual cleanup");
    OsPollJoysticks(1);
    Check(Drain().empty(), "no manufactured input after cleanup");
    Check(SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 2, 0) == 0 && OsJoystickOpen(0) == 0,
        "leave fixture for input-owner teardown");
}
}

int main() {
    if (access("/sys/class", F_OK) == 0 || access("/dev/input", F_OK) == 0) {
        std::fprintf(stderr, "Refusing host device access; run under bwrap with private /dev and empty /sys.\n");
        return 1;
    }
    // Run under bwrap with private /dev and empty /sys, not on host devices.
    // Hints additionally disable physical HID and Linux joystick backends.
    Check(!OsClipboardGetString() && !OsClipboardPutString("uninitialized"), "clipboard failure without video");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_LINUX_JOYSTICK_CLASSIC, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DEVICE, "/nonexistent/sdl-services-fixture");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError()); return 1;
    }
    auto window = SDL_CreateWindow("fixture", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
    Check(window != nullptr, "dummy window");
    OsGuiSetGxWindow(window);
    OsInputInitialize();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    Clipboard();
    IME(window);
    JoystickRemapping();
    Joysticks();
    Check(OsWindowProc(window, 0xffffffff, 0, 0) == 0, "non-applicable legacy default hook");
    OsInputDestroy();
    Check(!SDL_IsTextInputActive(), "input owner stops text");
    char name[16];
    Check(!OsJoystickGetName(0, name, sizeof(name)), "input owner closes joystick handles");
    Check(SDL_JoystickDetachVirtual(0) == 0, "teardown fixture cleanup");
    OsGuiSetGxWindow(nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("SdlServicesTest: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
