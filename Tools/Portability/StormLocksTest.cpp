#include <array>
#include <cstdio>
#include <thread>
#include "Engine/Source/Storm/H/SCritSect.h"

int main() {
    CCritSect lock;
    lock.Enter();
    lock.Enter(1);
    lock.Leave(1);
    lock.Leave();

    int count = 0;
    std::array<std::thread, 8> threads;
    for (auto& thread : threads) {
        thread = std::thread([&] {
            for (unsigned i = 0; i < 5000; ++i) {
                // Both modes must stay exclusive, like the Windows critical section.
                lock.Enter(i & 1);
                lock.Enter();
                ++count;
                lock.Leave();
                lock.Leave(i & 1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (count != 40000) {
        std::fprintf(stderr, "FAIL: expected 40000 protected increments, got %d\n", count);
        return 1;
    }
    std::puts("PASS: recursive locking and mutual exclusion across both API modes");
    return 0;
}
