#include "Engine/Source/Storm/H/storm.h"

namespace {
// Preserve the client error-code domain independently of libc errno. A later
// successful syscall must not erase the last error reported by a Storm API.
thread_local uint32_t s_lastError;
}

uint32_t STORMAPI SErrGetLastError() {
    return s_lastError;
}

void STORMAPI SErrSetLastError(uint32_t error) {
    s_lastError = error;
}
