#include "Engine/Source/Os/OsTcp.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
int main() {
    struct Unaligned { char prefix; NETADDR address; } value{};
    value.prefix = 'x';
    const uint32_t ip = htonl(0x7f000001u);
    OsNetAddrMake(ip, 3724, &value.address);
    uint16_t port = 0;
    Check(OsNetAddrGetAddress(&value.address, &port) == ip && port == 3724,
          "unaligned address preserves network-order IPv4 and host-order port");
    Check(value.prefix == 'x', "address write respects bounds");
    char text[64];
    Check(OsNetAddrToStr(&value.address, text, sizeof(text)) == text &&
          std::strcmp(text, "127.0.0.1:3724") == 0, "IPv4 endpoint formatting");
    char shortText[3] = {'x', 'x', 'x'};
    OsNetAddrToStr(&value.address, shortText, sizeof(shortText));
    Check(shortText[2] == '\0', "bounded formatting terminates");
    errno = 0;
    OsNetAddrMake(ip, 0, nullptr);
    Check(errno == EINVAL, "null destination rejected");
    port = 1;
    Check(!OsNetAddrGetAddress(nullptr, &port) && !port && errno == EINVAL,
          "invalid read clears port");
    value.address = {};
    Check(!OsNetAddrGetAddress(&value.address, nullptr) && errno == EAFNOSUPPORT,
          "unsupported family rejected");
    OsNetAddrToStr(&value.address, text, sizeof(text));
    Check(!text[0], "invalid address produces no fabricated endpoint");
    OsNetPump(0);
    std::puts("PASS: native IPv4 address storage/formatting and empty legacy pump; no network traffic");
}
