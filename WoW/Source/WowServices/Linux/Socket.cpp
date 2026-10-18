#include "WoW/Source/WowServices/PosixSocket.h"
#undef send

ssize_t WowSocketSend(int fd, const void* data, size_t size, int flags) {
    ssize_t result;
    do { result = ::send(fd, data, size, flags | MSG_NOSIGNAL); } while (result < 0 && errno == EINTR);
    return result;
}
