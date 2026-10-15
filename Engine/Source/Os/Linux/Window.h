#ifndef ENGINE_SOURCE_OS_LINUX_WINDOW_H
#define ENGINE_SOURCE_OS_LINUX_WINDOW_H

#include <cstdint>
#include <SDL.h>

struct tagRECT {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

// The input pump dispatches on the video thread. SDL event watches can run on
// producer threads, which must not reset or release a graphics device.
using OsNativeWindowEventProc = void (*)(void* context, const SDL_WindowEvent& event);
void OsGuiSetNativeWindowEventProc(OsNativeWindowEventProc proc, void* context);
void OsGuiDispatchNativeWindowEvent(const SDL_WindowEvent& event);

#endif
