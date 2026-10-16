#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>

namespace {
bool ReadAddress(const NETADDR* address, sockaddr_in& native) {
    static_assert(sizeof(NETADDR) == sizeof(sockaddr_in), "IPv4 address storage size");
    if (!address) { errno = EINVAL; return false; }
    // NETADDR's byte storage need not have sockaddr_in alignment.
    std::memcpy(&native, address, sizeof(native));
    if (native.sin_family != AF_INET) { errno = EAFNOSUPPORT; return false; }
    return true;
}
}

void OsNetAddrMake(uint32_t address, uint16_t port, NETADDR* result) {
    if (!result) { errno = EINVAL; return; }
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_port = htons(port);
    native.sin_addr.s_addr = address;
    std::memcpy(result, &native, sizeof(native));
}

uint32_t OsNetAddrGetAddress(const NETADDR* address, uint16_t* port) {
    if (port) *port = 0;
    sockaddr_in native{};
    if (!ReadAddress(address, native)) return 0;
    if (port) *port = ntohs(native.sin_port);
    return native.sin_addr.s_addr;
}

const char* OsNetAddrToStr(const NETADDR* address, char* buffer, uint32_t size) {
    if (!buffer || !size) { errno = EINVAL; return buffer; }
    buffer[0] = '\0';
    sockaddr_in native{};
    if (!ReadAddress(address, native)) return buffer;
    char text[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &native.sin_addr, text, sizeof(text))) return buffer;
    SStrPrintf(buffer, size, "%s:%u", text, ntohs(native.sin_port));
    return buffer;
}

void OsNetPump(uint32_t timeout) {
    // No legacy IOCP endpoints exist on Linux. Preserve its no-endpoint wait;
    // real WowConnection transport owns independent native poll threads.
    OsSleep(timeout);
}
