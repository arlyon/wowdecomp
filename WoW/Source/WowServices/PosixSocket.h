#ifndef WOW_SOURCE_WOWSERVICES_POSIXSOCKET_H
#define WOW_SOURCE_WOWSERVICES_POSIXSOCKET_H
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
static_assert(sizeof(sockaddr_in) == 16, "NETADDR stores IPv4 socket addresses");
inline int WSAGetLastError() { return errno == EAGAIN ? EWOULDBLOCK : errno; }
constexpr int WSAEINTR = EINTR;
constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;
inline int closesocket(int fd) { return close(fd); } // Never retry close: fd may already be reused.
inline bool WowSetNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
// OS leaf: Linux uses MSG_NOSIGNAL; Darwin needs its own SIGPIPE policy.
// Preserve EINTR retry and the caller's nonblocking/error semantics.
ssize_t WowSocketSend(int fd, const void* data, size_t size, int flags);
#define send WowSocketSend
#endif
