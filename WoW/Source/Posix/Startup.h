#ifndef WOW_SOURCE_POSIX_STARTUP_H
#define WOW_SOURCE_POSIX_STARTUP_H

// Configure the calling thread; the SDL/message-loop thread must remain main.
bool NativeStartupInitializeFloatingPoint();
bool NativeStartupEnsureStack();

#endif
