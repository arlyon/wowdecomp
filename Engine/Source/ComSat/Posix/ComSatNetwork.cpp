#if defined(WOWDECOMP_ARCH_64) && !defined(_WIN32)

#include "Engine/Source/ComSat/ComSatNetwork.h"
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int Descriptor(uintptr_t handle) {
    return handle <= INT_MAX ? static_cast<int>(handle) : -1;
}

// Address stores the original IPv4 wire fields, not a native sockaddr.
bool NativeAddress(const ComSat::Network::Address& address, sockaddr_in& native) {
    if (address.m_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return false;
    }
    if (address.m_length != 16) {
        errno = EINVAL;
        return false;
    }
    native = {};
    native.sin_family = AF_INET;
    native.sin_port = address.m_port;
    native.sin_addr.s_addr = address.m_ip;
    return true;
}
}

void ComSat::Network::Address::SetPort(int32_t port) {
    this->m_port = htons(static_cast<uint16_t>(port));
}

int32_t ComSat::Network::Address::GetPort() const {
    return ntohs(this->m_port);
}

void ComSat::Network::Address::SetAddress(const char* address) {
    const char* colon = strchr(address, ':');
    if (colon) {
        if (strlen(address) < 128) {
            char host[128];
            strcpy(host, address);
            host[colon - address] = '\0';
            this->m_ip = inet_addr(host);
            this->m_port = htons(static_cast<uint16_t>(strtol(colon + 1, nullptr, 10)));
        }
    } else {
        this->m_ip = inet_addr(address);
    }
}

ComSat::Network::SocketPosix::SocketPosix() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    this->m_socket = static_cast<uintptr_t>(-1);
    if (fd == -1) {
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        int error = errno;
        close(fd);
        errno = error;
        return;
    }
    int value = 0x8000;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
    this->m_socket = static_cast<uintptr_t>(fd);
}

ComSat::Network::SocketPosix::~SocketPosix() {
    int fd = Descriptor(this->m_socket);
    if (fd != -1) {
        close(fd);
    }
}

bool ComSat::Network::SocketPosix::Bind(int32_t port) {
    Address address;
    address.SetPort(port);
    sockaddr_in native;
    if (!NativeAddress(address, native)) {
        return false;
    }
    int fd = Descriptor(this->m_socket);
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    return bind(fd, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) != -1;
}

int32_t ComSat::Network::SocketPosix::SendTo(const Address& address, const uint8_t* data, uint32_t size) {
    sockaddr_in native;
    if (!NativeAddress(address, native)) {
        return -1;
    }
    return sendto(Descriptor(this->m_socket), data, size, 0,
                  reinterpret_cast<const sockaddr*>(&native), sizeof(native));
}

bool ComSat::Network::SocketPosix::RecvFrom(Address& address, uint8_t* data, uint32_t& size) {
    memset(data, 0, size);
    sockaddr_in native = {};
    socklen_t length = sizeof(native);
    ssize_t received = recvfrom(Descriptor(this->m_socket), data, size, 0,
                                reinterpret_cast<sockaddr*>(&native), &length);
    if (received <= 0) {
        return false;
    }
    address.m_length = 16;
    address.m_family = native.sin_family;
    address.m_port = native.sin_port;
    address.m_ip = native.sin_addr.s_addr;
    memset(address.m_zero, 0, sizeof(address.m_zero));
    size = static_cast<uint32_t>(received);
    return true;
}

#endif
