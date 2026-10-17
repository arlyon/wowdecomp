#include <SDL.h>
#include <pthread.h>
#include <unistd.h>

namespace {
void* QuitSoon(void*) {
    sleep(8);
    SDL_Event event{};
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
    return nullptr;
}

// Exercise the real normal-quit/static-finalization path, not a signal handler
// or _exit(). A crash is a failed test; failure to handle the event times out.
__attribute__((constructor)) void InitializeProbe() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, QuitSoon, nullptr) == 0)
        pthread_detach(thread);
}
}
