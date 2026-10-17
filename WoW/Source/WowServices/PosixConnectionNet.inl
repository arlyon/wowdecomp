#include "WoW/Source/WowServices/WowConnectionNet.h"
#include "WoW/Source/WowServices/PosixSocket.h"
#include <poll.h>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <exception>

namespace {
struct WowPollWake {
    int readFd = -1;
    int writeFd = -1;
};
}

void WowConnectionNet::PlatformInit(bool useEngine) {
    this->m_event = nullptr;
    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC)) {
        throw std::runtime_error("WowConnectionNet wake pipe creation failed");
    }
    try {
        this->m_event = new WowPollWake{fds[0], fds[1]};
    } catch (...) {
        close(fds[0]);
        close(fds[1]);
        throw;
    }
}

void WowConnectionNet::PlatformWorkerReady() {
    auto* wake = static_cast<WowPollWake*>(this->m_event);
    if (!wake) return;
    char byte = 0;
    ssize_t result;
    do { result = write(wake->writeFd, &byte, 1); } while (result < 0 && errno == EINTR);
    // EAGAIN means a wake is already queued. Resources live until Stop joins.
}
void WowConnectionNet::PlatformStop() { this->PlatformWorkerReady(); }
void WowConnectionNet::PlatformChangeState(WowConnection*, WOW_CONN_STATE) { this->PlatformWorkerReady(); }
void WowConnectionNet::PlatformRemove(WowConnection*) { this->PlatformWorkerReady(); }
void WowConnectionNet::PlatformDestruct(WowConnection*) {}
void WowConnectionNet::PlatformAdd(WowConnection* connection) {
    int enabled = 1;
    setsockopt(connection->m_sock, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    this->PlatformWorkerReady();
}
void WowConnectionNet::PlatformDestroy() {
    auto* wake = static_cast<WowPollWake*>(this->m_event);
    if (!wake) return;
    close(wake->readFd);
    close(wake->writeFd);
    delete wake;
    this->m_event = nullptr;
}

void WowConnectionNet::PlatformRun() {
    __atomic_store_n(&this->m_networkStarted, true, __ATOMIC_RELEASE);
    auto* wake = static_cast<WowPollWake*>(this->m_event);
    while (!__atomic_load_n(&this->m_stop, __ATOMIC_ACQUIRE)) {
        std::vector<WowConnection*> connections;
        std::vector<WOW_CONN_STATE> states;
        std::vector<pollfd> descriptors{{wake->readFd, POLLIN, 0}};
        // Never take connection locks under the list lock: removal takes them in reverse order.
        this->m_connectionsLock.Enter();
        for (auto* c = this->m_connections.Head(); c; c = this->m_connections.Next(c)) {
            c->AddRef();
            connections.push_back(c);
        }
        this->m_connectionsLock.Leave();
        int timeout = 500;
        for (auto* c : connections) {
            c->m_lock.Enter();
            short events = 0;
            if (!__atomic_load_n(&c->m_serviceCount, __ATOMIC_ACQUIRE)) {
                switch (c->m_connState) {
                    case WOWC_CONNECTING: events = POLLOUT; break;
                    case WOWC_LISTENING: events = POLLIN; break;
                    case WOWC_CONNECTED:
                        events = POLLIN;
                        if (c->m_sendList.Head() || c->m_notifyWriteReady) events |= POLLOUT;
                        if (c->m_lagTestRecvList.Head()) timeout = 10;
                        break;
                    case WOWC_DISCONNECTING: timeout = 0; break;
                    default: break;
                }
            }
            states.push_back(c->m_connState);
            descriptors.push_back({events ? c->m_sock : -1, events, 0});
            c->m_lock.Leave();
        }
        int result = poll(descriptors.data(), descriptors.size(), timeout);
        int pollError = result < 0 ? errno : 0;
        if (result >= 0) {
            if (descriptors[0].revents & POLLIN) {
                char bytes[128];
                while (read(wake->readFd, bytes, sizeof(bytes)) > 0) {}
            }
            for (size_t i = 0; i < connections.size(); ++i) {
                auto* c = connections[i];
                c->m_lock.Enter();
                const auto& descriptor = descriptors[i + 1];
                uint32_t flags = 0;
                if (!__atomic_load_n(&c->m_serviceCount, __ATOMIC_ACQUIRE)) {
                    if (c->m_connState == WOWC_DISCONNECTING) flags = 8;
                    else if (c->m_sock == descriptor.fd && descriptor.fd >= 0 && c->m_connState == states[i]) {
                        if (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) flags |= 2;
                        if (descriptor.revents & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) flags |= 1;
                        if (c->m_lagTestRecvList.Head()) flags |= 2;
                    }
                }
                if (flags) this->SignalWorker(c, flags);
                c->m_lock.Leave();
            }
        }
        for (auto* c : connections) c->Release();
        if (result < 0 && pollError != EINTR) {
            std::fprintf(stderr, "WowConnectionNet poll failed: errno=%d\n", pollError);
            std::terminate(); // Never leave an apparently running but unserviced transport.
        }
    }
}
