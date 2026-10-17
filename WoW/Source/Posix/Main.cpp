#include "WoW/Source/Client.h"
#include "WoW/Source/Native/Startup.h"
#include <cstdio>
#include <SDL.h>

int main() {
    if (!NativeStartupEnsureStack()) {
        std::perror("Wow: cannot provide 8 MiB main-thread stack");
        return 1;
    }

    StormRtlInitialize();
    if (!NativeStartupInitializeFloatingPoint()) {
        std::perror("Wow: unsupported floating-point CPU");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "Wow: SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    struct SdlLifetime { ~SdlLifetime() { SDL_Quit(); } } sdl;
    return CommonMain();
}
