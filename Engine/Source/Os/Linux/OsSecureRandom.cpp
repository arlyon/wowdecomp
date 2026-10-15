#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/random.h>
#include "Engine/Source/Os/OsSecureRandom.h"

void IOsSecureRandom(uint8_t* buffer, uint32_t size) {
    while (size) {
        const ssize_t count = getrandom(buffer, size, 0);
        if (count > 0) {
            buffer += count;
            size -= static_cast<uint32_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            // The API cannot report failure. Never supply weak or uninitialized
            // authentication randomness if the kernel entropy source fails.
            if (!count) {
                errno = EIO;
            }
            std::perror("OsSecureRandom: getrandom");
            std::abort();
        }
    }
}

void OsSecureRandom(uint8_t* buffer, uint32_t size) {
    IOsSecureRandom(buffer, size);
}
