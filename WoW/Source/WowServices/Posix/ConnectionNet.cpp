// POSIX implementation; the original Windows translation unit is unchanged.
#include "WoW/Source/WowServices/WowConnectionNet.h"

#include <stdexcept>
#include <cstdio>
#include <exception>
static thread_local WowConnectionNet* s_currentNetwork;

#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// VERIFY: 1491f4aa auto
// OFFSET: 0x00469060
static uint32_t STORMAPI NetworkThreadProc(void* param) {
    WowConnectionNet* network = static_cast<WowConnectionNet*>(param);

    s_currentNetwork = network;

    try {
        network->PlatformRun();
    } catch (...) {
        std::fprintf(stderr, "WowConnectionNet network thread threw an exception\n");
        std::terminate();
    }

    network->m_stopEvent.Set();

    return 0;
}

// VERIFY: d0e3b08a auto
// OFFSET: 0x00469300
static uint32_t STORMAPI WorkerThreadProc(void* param) {
    WowConnectionNet::WORKER* worker = static_cast<WowConnectionNet::WORKER*>(param);

    s_currentNetwork = worker->m_owner;

    try {
        worker->m_owner->RunWorker(worker->m_index);
    } catch (...) {
        std::fprintf(stderr, "WowConnectionNet worker callback threw an exception\n");
        std::terminate();
    }

    return 0;
}

// INLINED
WowConnectionNet::WORKER::WORKER() : m_event(0, 0) {
}

// VERIFY: 87918bed manual
// OFFSET: 0x00469320
void WowConnectionNet::Start() {

    int created = 0;
    try {
        for (int i = 0; i < this->m_numWorkers; ++i) {
            auto& worker = this->m_workers[i];
            worker.m_owner = this;
            worker.m_index = i;
            worker.m_connection = nullptr;
            worker.m_quit = false;
            worker.m_lastTime = OsGetAsyncTimeMs();
            char name[] = "NetThread";
            if (!SThread::Create(WorkerThreadProc, &worker, worker.m_thread, name, 0))
                throw std::runtime_error("WowConnectionNet worker creation failed");
            ++created;
        }
        __atomic_store_n(&this->m_networkStarted, false, __ATOMIC_RELEASE);
        char name[] = "Network";
        if (!SThread::Create(NetworkThreadProc, this, this->m_thread, name, 0))
            throw std::runtime_error("WowConnectionNet network thread creation failed");
        while (!__atomic_load_n(&this->m_networkStarted, __ATOMIC_ACQUIRE)) OsSleep(1);
    } catch (...) {
        for (int i = 0; i < created; ++i) {
            __atomic_store_n(&this->m_workers[i].m_quit, true, __ATOMIC_RELEASE);
            this->m_workers[i].m_event.Set();
        }
        for (int i = 0; i < created; ++i) this->m_workers[i].m_thread.Wait(0xFFFFFFFF);
        throw;
    }

}

// VERIFY: c90eb59b auto
// OFFSET: 0x00468FF0
void WowConnectionNet::Service(WowConnection* connection, uint32_t flags) {
    do {
        if (flags & 0x1) {
            connection->DoWrites();
        }

        if (flags & 0x2) {
            connection->DoReads();
        }

        if (flags & 0x4) {
            connection->DoExceptions();
        }

        if (flags & 0x8) {
            connection->DoDisconnect();
        }

        this->m_connectionsLock.Enter();
        flags = connection->m_serviceFlags;
        connection->m_serviceFlags = 0;
        this->m_connectionsLock.Leave();
    } while (flags);
}

// VERIFY: 3c6bf49d auto
// OFFSET: 0x00469160
void WowConnectionNet::RunWorker(int32_t index) {
    if (this->m_threadInit) {
        this->m_threadInit();
    }

    this->m_workerSemaphore.Signal(1);

    WowConnectionNet::WORKER& worker = this->m_workers[index];

    while (true) {
        do {
            worker.m_lastTime = OsGetAsyncTimeMs();
        } while (worker.m_event.Wait(1000));

        if (__atomic_load_n(&worker.m_quit, __ATOMIC_ACQUIRE)) {

            worker.m_lock.Enter();
            if (worker.m_connection) {
                __atomic_fetch_sub(&worker.m_connection->m_serviceCount, 1, __ATOMIC_RELEASE);
                worker.m_connection->Release();
                worker.m_connection = nullptr;
            }
            worker.m_lock.Leave();

            break;
        }

        uint32_t serviceFlags = worker.m_connection->m_serviceFlags;
        worker.m_connection->m_serviceFlags = 0;
        this->Service(worker.m_connection, serviceFlags);

        worker.m_lock.Enter();

        __atomic_fetch_add(reinterpret_cast<volatile int32_t*>(&worker.m_connection->m_serviceCount), -1, __ATOMIC_SEQ_CST);

        worker.m_connection->Release();
        worker.m_connection = nullptr;
        worker.m_lock.Leave();

        this->m_workerSemaphore.Signal(1);
        this->PlatformWorkerReady();
    }
}

// VERIFY: d5522788 manual
// OFFSET: 0x00469220
void WowConnectionNet::SignalWorker(WowConnection* connection, uint32_t flags) {
    if (this->m_workerSemaphore.Wait(500)) {
        return;
    }

    connection->m_refCount.Increment();
    connection->m_serviceFlags = flags;

    __atomic_fetch_add(reinterpret_cast<volatile int32_t*>(&connection->m_serviceCount), 1, __ATOMIC_SEQ_CST);

    for (int32_t i = 0; i < this->m_numWorkers; i++) {
        WowConnectionNet::WORKER& worker = this->m_workers[i];
        worker.m_lock.Enter();

        if (!worker.m_connection) {
            worker.m_connection = connection;
            worker.m_lock.Leave();
            worker.m_event.Set();
            return;
        }

        worker.m_lock.Leave();
    }
}

// VERIFY: e172a3d8 manual
// OFFSET: 0x004692C0
void WowConnectionNet::Delete(WowConnection* connection) {
    this->m_connectionsLock.Enter();

    if (!__atomic_load_n(&connection->m_refCount.m_value, __ATOMIC_ACQUIRE)) {

        delete connection;
    }

    this->m_connectionsLock.Leave();
}

// VERIFY: da8d942b manual
// OFFSET: 0x004695F0
WowConnectionNet::WowConnectionNet(int32_t numThreads, void (*threadInit)()) : m_stopEvent(1, 0), m_workerSemaphore(0, numThreads), m_serviceSemaphore(0, 0x7FFFFFFF) {
    this->m_numWorkers = numThreads;
    this->m_threadInit = threadInit;
    this->m_stop = false;
}

// VERIFY: 88d38822 manual
// OFFSET: 0x00469420
void WowConnectionNet::Add(WowConnection* connection) {
    this->m_connectionsLock.Enter();

    if (!this->m_connections.IsLinked(connection)) {
        this->m_connections.LinkToTail(connection);
        this->PlatformAdd(connection);
    }

    this->m_connectionsLock.Leave();
}

// VERIFY: f12734a2 manual
// OFFSET: 0x00469470
void WowConnectionNet::Remove(WowConnection* connection) {
    this->m_connectionsLock.Enter();

    if (this->m_connections.IsLinked(connection)) {
        this->m_connections.UnlinkNode(connection);
    }

    this->PlatformRemove(connection);
    this->m_connectionsLock.Leave();
}

// VERIFY: eb055e2d manual
// OFFSET: 0x00469080
void WowConnectionNet::Stop() {

    // Destruction from a network callback cannot join its own thread.
    if (s_currentNetwork == this) throw std::logic_error("WowConnectionNet self-shutdown is unsupported");
    __atomic_store_n(&this->m_stop, true, __ATOMIC_RELEASE);
    this->PlatformStop();
    this->m_thread.Wait(0xFFFFFFFF);
    for (int i = 0; i < this->m_numWorkers; ++i) {
        __atomic_store_n(&this->m_workers[i].m_quit, true, __ATOMIC_RELEASE);
        this->m_workers[i].m_event.Set();
    }
    for (int i = 0; i < this->m_numWorkers; ++i) this->m_workers[i].m_thread.Wait(0xFFFFFFFF);

}

// VERIFY: 91633125 manual
// OFFSET: 0x004694C0
WowConnectionNet::~WowConnectionNet() {
}
