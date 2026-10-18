#include "Engine/Source/ComSat/ComSatNetwork.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno);
        std::exit(1);
    }
}
int Fd(const ComSat::Network::SocketPosix& socket) {
    return static_cast<int>(socket.m_socket);
}
ComSat::Network::Address Destination(const ComSat::Network::SocketPosix& socket) {
    sockaddr_in native = {};
    socklen_t length = sizeof(native);
    Check(getsockname(Fd(socket), reinterpret_cast<sockaddr*>(&native), &length) == 0, "getsockname");
    ComSat::Network::Address address;
    address.SetAddress("127.0.0.1");
    address.SetPort(ntohs(native.sin_port));
    return address;
}
}

int main() {
    using namespace ComSat::Network;
    static_assert(sizeof(Address) == 32, "Address layout");
    Address address;
    Check(address.m_length == 16 && address.m_family == AF_INET && address.m_ip == 0, "default address");
    address.SetAddress("127.0.0.1:54321");
    Check(address.GetPort() == 54321 && address.m_ip == htonl(0x7f000001), "IPv4 host:port");
    address.SetAddress("127.0.0.2");
    Check(address.GetPort() == 54321 && address.m_ip == htonl(0x7f000002), "host preserves port");
    address.SetPort(-1);
    Check(address.GetPort() == 65535, "port narrowing");
    address.SetAddress("not-an-address");
    Check(address.m_ip == INADDR_NONE, "invalid address without DNS");
    address.SetAddress("255.255.255.255");
    Check(address.m_ip == INADDR_BROADCAST, "broadcast parsing only");
    Address copy = address;
    Check(copy == address, "address equality");
    copy.SetPort(7);
    Check(!(copy == address), "different ports");
    char longAddress[130];
    memset(longAddress, '1', sizeof(longAddress));
    longAddress[1] = ':';
    longAddress[129] = 0;
    copy = address;
    address.SetAddress(longAddress);
    Check(address == copy, "overlong host:port unchanged");

    int closedFd;
    {
        SocketPosix sender, receiver;
        Check(Fd(sender) >= 0 && Fd(receiver) >= 0, "create UDP sockets");
        Check((fcntl(Fd(receiver), F_GETFL) & O_NONBLOCK) != 0, "nonblocking");
        Check(sender.Bind(0) && receiver.Bind(0), "bind ephemeral UDP ports");
        Address target = Destination(receiver);
        uint8_t buffer[32];
        uint32_t size = sizeof(buffer);
        Address source;
        source.SetAddress("127.0.0.2:19");
        Address original = source;
        memset(buffer, 0xff, sizeof(buffer));
        Check(!receiver.RecvFrom(source, buffer, size), "empty receive fails");
        Check((errno == EAGAIN || errno == EWOULDBLOCK) && size == sizeof(buffer) && source == original,
              "would-block preserves outputs");
        for (uint8_t byte : buffer) Check(byte == 0, "receive buffer cleared");
        const uint8_t payload[] = {0, 1, 2, 255};
        Check(sender.SendTo(target, payload, sizeof(payload)) == sizeof(payload), "send loopback UDP");
        pollfd ready = {Fd(receiver), POLLIN, 0};
        Check(poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN), "bounded receive wait");
        Check(receiver.RecvFrom(source, buffer, size), "receive loopback UDP");
        Check(size == sizeof(payload) && memcmp(buffer, payload, size) == 0, "payload preserved");
        Check(source == Destination(sender), "source address and port");
        Check(sender.SendTo(target, payload, sizeof(payload)) == sizeof(payload), "send for truncation");
        ready.revents = 0;
        Check(poll(&ready, 1, 1000) == 1, "truncation datagram arrives");
        size = 2;
        Check(receiver.RecvFrom(source, buffer, size) && size == 2 && memcmp(buffer, payload, 2) == 0,
              "UDP receive truncates to caller capacity");
        uint8_t oversized[65508] = {};
        Check(sender.SendTo(target, oversized, sizeof(oversized)) == -1 && errno == EMSGSIZE,
              "oversized UDP send fails");
        Check(sender.SendTo(target, payload, 0) == 0, "zero datagram send");
        ready.revents = 0;
        Check(poll(&ready, 1, 1000) == 1, "zero datagram arrives");
        size = sizeof(buffer);
        Check(!receiver.RecvFrom(source, buffer, size) && size == sizeof(buffer), "zero datagram returns false as original");
        target.m_family = AF_INET6;
        Check(sender.SendTo(target, payload, sizeof(payload)) == -1 && errno == EAFNOSUPPORT, "IPv6 unsupported by IPv4 contract");
        target = Destination(receiver);
        target.m_length = 32;
        Check(sender.SendTo(target, payload, sizeof(payload)) == -1 && errno == EINVAL, "invalid address length");
        Check(!receiver.Bind(0) && errno == EINVAL, "second bind fails");
        closedFd = Fd(receiver);
    }
    Check(fcntl(closedFd, F_GETFD) == -1 && errno == EBADF, "destructor closes descriptor");

    // Descriptor zero is a valid socket, independent of whether stdin was open.
    int savedStdin = dup(STDIN_FILENO);
    close(STDIN_FILENO);
    {
        SocketPosix zero;
        Check(Fd(zero) == 0 && zero.Bind(0), "descriptor zero works");
    }
    Check(fcntl(0, F_GETFD) == -1 && errno == EBADF, "descriptor zero closed");
    if (savedStdin != -1) {
        Check(dup2(savedStdin, STDIN_FILENO) == STDIN_FILENO, "restore stdin");
        close(savedStdin);
    }

    // Force constructor failure without exhausting or allocating descriptors.
    rlimit limits;
    Check(getrlimit(RLIMIT_NOFILE, &limits) == 0, "get descriptor limit");
    rlimit restricted = limits;
    restricted.rlim_cur = 0;
    Check(setrlimit(RLIMIT_NOFILE, &restricted) == 0, "restrict descriptors");
    {
        SocketPosix failed;
        Check(failed.m_socket == static_cast<uintptr_t>(-1), "failed constructor invalid handle");
        Check(!failed.Bind(0) && errno == EBADF, "failed socket bind");
        uint8_t data[1];
        uint32_t size = sizeof(data);
        Address target;
        target.SetAddress("127.0.0.1:1");
        Check(failed.SendTo(target, data, size) == -1 && errno == EBADF, "failed socket send");
        Check(!failed.RecvFrom(target, data, size) && errno == EBADF, "failed socket receive");
    }
    Check(setrlimit(RLIMIT_NOFILE, &limits) == 0, "restore descriptor limit");
    std::puts("ComSat IPv4 UDP tests passed (IPv6 not supported by API)");
}
