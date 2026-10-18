#include "WoW/Source/WowServices/WowConnection.h"
#include "WoW/Source/WowServices/WowConnectionNet.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <csignal>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;
// Evaluate syscall/setup checks even in Release builds with NDEBUG.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        std::abort(); \
    } \
} while (false)
// Inject only real syscall errors; every successful byte transfer is the actual kernel stream.
static std::atomic<int> failCreateAt{0};
static std::atomic<int> sendInterrupts{0}, recvInterrupts{0}, pollInterrupts{0};
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*entry)(void*), void* argument) {
    int value = failCreateAt.load();
    while (value > 0) {
        if (failCreateAt.compare_exchange_weak(value, value - 1)) {
            if (value == 1) return EAGAIN;
            break;
        }
    }
    return __real_pthread_create(thread, attributes, entry, argument);
}
extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" int __real_poll(pollfd*, nfds_t, int);
static bool interrupt(std::atomic<int>& count) {
    int value = count.load();
    while (value > 0) if (count.compare_exchange_weak(value, value - 1)) { errno = EINTR; return true; }
    return false;
}
extern "C" ssize_t __wrap_send(int fd, const void* data, size_t size, int flags) {
    CHECK(flags & MSG_NOSIGNAL);
    if (interrupt(sendInterrupts)) return -1;
    return __real_send(fd, data, size, flags);
}
extern "C" ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
    if (interrupt(recvInterrupts)) return -1;
    return __real_recv(fd, data, size, flags);
}
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (interrupt(pollInterrupts)) return -1;
    return __real_poll(fds, count, timeout);
}
static void waitFor(const std::atomic<int>& value, int target) {
    auto end = std::chrono::steady_clock::now() + 5s;
    while (value.load() < target && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    CHECK(value.load() >= target);
}
static void waitForResponse(WowConnection* connection, WowConnectionResponse* expected) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        connection->m_responseLock.Enter();
        if (connection->m_responseRef == 0) {
            CHECK(connection->m_response == expected);
            connection->m_responseLock.Leave();
            return;
        }
        connection->m_responseLock.Leave();
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}
struct Response : WowConnectionResponse {
    std::atomic<int> connected{0}, disconnected{0}, failed{0}, bytes{0}, selfRejected{0};
    bool selfStop = false;
    std::atomic<uint16_t> reconnectPort{0};
    std::atomic<bool> hold{false}, entered{false}, resume{false};
    void WCMessageReady(WowConnection*, uint32_t, CDataStore*) override { CHECK(false); }
    void WCConnected(WowConnection*, WowConnection*, uint32_t, const NETCONNADDR*) override { ++connected; }
    void WCCantConnect(WowConnection*, uint32_t, const NETCONNADDR*) override { ++failed; }
    void WCDisconnected(WowConnection* c, uint32_t, const NETCONNADDR*) override {
        if (reconnectPort) {
            CHECK(c->m_sock == -1);
            auto port = reconnectPort.exchange(0);
            CHECK(c->Connect(htonl(INADDR_LOOPBACK), port, 1000));
        }
        ++disconnected;
    }
    void WCDataReady(WowConnection* c, uint32_t, uint8_t*, int32_t size) override {
        if (selfStop) {
            try { WowConnection::DestroyOsNet(); CHECK(false); }
            catch (const std::logic_error&) { ++selfRejected; }
            selfStop = false;
            c->SetResponse(this, true);
        }
        if (hold.exchange(false)) {
            c->AcquireResponseRef();
            entered = true;
            while (!resume.load()) std::this_thread::sleep_for(1ms);
            c->ReleaseResponseRef();
            CHECK(c->m_response == this); // Pending replacement waits for outermost callback.
        }
        bytes += size;
    }
};
static int listener(uint16_t& port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t length = sizeof(addr);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
    port = ntohs(addr.sin_port);
    CHECK(listen(fd, 4) == 0);
    return fd;
}
int main() {
    alarm(30);
    CHECK(!WowConnection::InitOsNet(nullptr, nullptr, 0, false));
    for (int failure : {1, 3, 5}) {
        failCreateAt = failure; // First worker, second worker, then network launcher.
        CHECK(!WowConnection::InitOsNet(nullptr, nullptr, 2, false));
        CHECK(!WowConnection::s_network);
    }
    CHECK(WowConnection::InitOsNet(nullptr, nullptr, 2, false));
    uint16_t port;
    int server = listener(port);
    Response response;
    response.selfStop = true;
    auto* c = new WowConnection(&response, nullptr);
    c->SetType(WOWC_TYPE_STREAM);
    c->Connect(htonl(INADDR_LOOPBACK), port, 1000);
    pollfd incoming{server, POLLIN, 0};
    CHECK(poll(&incoming, 1, 3000) == 1);
    int peer = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(peer >= 0);
    waitFor(response.connected, 1);
    char peerAddress[80];
    CHECK(c->PeerAddrToStr(peerAddress, sizeof(peerAddress)) == peerAddress);
    char expectedAddress[80];
    std::snprintf(expectedAddress, sizeof(expectedAddress), "127.0.0.1:%u", port);
    CHECK(std::string(peerAddress) == expectedAddress);
    try { WowConnection::DestroyOsNet(); CHECK(false); }
    catch (const std::logic_error&) {}

    recvInterrupts = 2;
    pollInterrupts = 1;
    CHECK(send(peer, "hello", 5, MSG_NOSIGNAL) == 5);
    waitFor(response.bytes, 5);
    waitFor(response.selfRejected, 1);
    auto heldCallback = [&] {
        response.entered = false;
        response.resume = false;
        response.hold = true;
        CHECK(send(peer, "x", 1, MSG_NOSIGNAL) == 1);
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!response.entered.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(response.entered.load());
    };
    heldCallback();
    c->SetResponse(nullptr, true);
    response.resume = true;
    waitFor(response.bytes, 6);
    waitForResponse(c, nullptr); // Observe deferred unregister before any new update.
    c->SetResponse(&response, false);
    Response replacement;
    heldCallback();
    c->SetResponse(&replacement, true);
    response.resume = true;
    waitFor(response.bytes, 7);
    waitForResponse(c, &replacement); // Must be applied by the outermost release itself.
    CHECK(send(peer, "new", 3, MSG_NOSIGNAL) == 3);
    waitFor(replacement.bytes, 3);
    c->SetResponse(&response, false);
    heldCallback();
    std::atomic<bool> updateFinished{false};
    std::thread update([&] { c->SetResponse(nullptr, false); updateFinished = true; });
    std::this_thread::sleep_for(20ms);
    CHECK(!updateFinished.load());
    response.resume = true;
    update.join();
    CHECK(updateFinished.load());
    waitForResponse(c, nullptr);
    c->SetResponse(&response, false);
    // Force partial sends and EAGAIN, then drain the actual shared send queue.
    int small = 4096;
    CHECK(setsockopt(c->m_sock, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    std::vector<uint8_t> payload(2 * 1024 * 1024, 0x5a);
    sendInterrupts = 2;
    CHECK(c->Send(payload.data(), payload.size(), true) == 1);
    CHECK(!c->Connect(htonl(INADDR_LOOPBACK), port, 1000)); // Must not discard a live queue.
    size_t received = 0;
    auto end = std::chrono::steady_clock::now() + 5s;
    uint8_t buffer[16384];
    while (received < payload.size() && std::chrono::steady_clock::now() < end) {
        pollfd ready{peer, POLLIN, 0};
        CHECK(poll(&ready, 1, 100) >= 0);
        ssize_t count = recv(peer, buffer, sizeof(buffer), 0);
        if (count > 0) {
            for (ssize_t i = 0; i < count; ++i) CHECK(buffer[i] == 0x5a);
            received += count;
        } else CHECK(count < 0 && (errno == EAGAIN || errno == EINTR));
    }
    CHECK(received == payload.size());
    response.reconnectPort = port;
    close(peer);
    waitFor(response.disconnected, 1);
    waitFor(response.connected, 2);
    incoming.revents = 0;
    CHECK(poll(&incoming, 1, 3000) == 1);
    peer = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(peer >= 0);
    CHECK(send(peer, "again", 5, MSG_NOSIGNAL) == 5);
    waitFor(response.bytes, 13);
    close(peer);
    waitFor(response.disconnected, 2);
    c->SetResponse(nullptr, false);
    c->Release();
    close(server);
    // Refused loopback connect: reserve but do not listen on the local endpoint.
    server = listener(port);
    close(server);
    Response refused;
    c = new WowConnection(&refused, nullptr);
    c->SetType(WOWC_TYPE_STREAM);
    CHECK(c->Connect("127.0.0.1", port, 1000));
    waitFor(refused.failed, 1);
    c->SetResponse(nullptr, false);
    c->Release();
    // Adopt FD0 and a descriptor beyond FD_SETSIZE. poll must handle both without fd_set.
    for (int minimum : {0, 2048}) {
        server = listener(port);
        int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
        CHECK(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        peer = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        int saved = minimum == 0 ? dup(0) : -1;
        int adopted = minimum == 0 ? dup2(peer, 0) : fcntl(peer, F_DUPFD_CLOEXEC, minimum);
        CHECK(adopted >= minimum);
        close(peer);
        WowConnection::MarkSocketId(adopted);
        Response adoptedResponse;
        c = new WowConnection(adopted, nullptr, &adoptedResponse);
        c->SetType(WOWC_TYPE_STREAM);
        WowConnection::s_network->Add(c);
        CHECK(send(client, "fd", 2, MSG_NOSIGNAL) == 2);
        waitFor(adoptedResponse.bytes, 2);
        c->Disconnect();
        waitFor(adoptedResponse.disconnected, 1);
        c->SetResponse(nullptr, false);
        c->Release();
        close(client); close(server);
        if (saved >= 0) { CHECK(dup2(saved, 0) == 0); close(saved); }
    }
    server = listener(port);
    Response reset;
    c = new WowConnection(&reset, nullptr);
    c->SetType(WOWC_TYPE_STREAM);
    CHECK(c->Connect(htonl(INADDR_LOOPBACK), port, 1000));
    incoming = {server, POLLIN, 0};
    CHECK(poll(&incoming, 1, 3000) == 1);
    peer = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(peer >= 0);
    waitFor(reset.connected, 1);
    CHECK(setsockopt(c->m_sock, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    CHECK(c->Send(payload.data(), payload.size(), true) == 1);
    reset.reconnectPort = port;
    linger abrupt{1, 0};
    CHECK(setsockopt(peer, SOL_SOCKET, SO_LINGER, &abrupt, sizeof(abrupt)) == 0);
    close(peer);
    waitFor(reset.disconnected, 1);
    waitFor(reset.connected, 2);
    incoming = {server, POLLIN, 0};
    CHECK(poll(&incoming, 1, 3000) == 1);
    peer = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(peer >= 0);
    c->m_lock.Enter();
    CHECK(c->m_sendList.Head() == nullptr && c->m_sendDepth == 0 && c->m_sendBytes == 0);
    c->m_lock.Leave();
    pollfd noStale{peer, POLLIN, 0};
    CHECK(poll(&noStale, 1, 50) == 0);
    uint8_t fresh[] = {'n', 'e', 'w'};
    CHECK(c->Send(fresh, sizeof(fresh), true) == 0);
    noStale.revents = 0;
    CHECK(poll(&noStale, 1, 3000) == 1);
    CHECK(recv(peer, buffer, sizeof(buffer), 0) == 3);
    CHECK(buffer[0] == 'n' && buffer[1] == 'e' && buffer[2] == 'w');
    close(peer);
    waitFor(reset.disconnected, 2);
    c->SetResponse(nullptr, false);
    c->Release();
    close(server);
    // Release removes caller ownership immediately; snapshot/service references drain asynchronously.
    auto shutdownDeadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        try { WowConnection::DestroyOsNet(); break; }
        catch (const std::logic_error&) {
            CHECK(std::chrono::steady_clock::now() < shutdownDeadline);
            std::this_thread::sleep_for(1ms);
        }
    }
    puts("PASS: loopback connect/backpressure, EINTR, callback refs/reconnect, refused connect, EOF/reset, FD0/high-fd, launch failure unwind and joined shutdown");
}
