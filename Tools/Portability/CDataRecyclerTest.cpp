#include "Engine/Source/Base/CDataRecycler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
void MultipleBlocks() {
    CDataRecycler pool(3, 48);
    std::set<void*> originals;
    for (unsigned i = 0; i < 48; ++i) {
        uint32_t bytes = 0;
        void* data = pool.AllocData(64, &bytes, __FILE__, __LINE__);
        Check(data && bytes == 64, "real allocation");
        std::memset(data, static_cast<int>(i), bytes);
        Check(originals.insert(data).second, "distinct initial allocations");
        pool.PutData(data, bytes, __FILE__, __LINE__);
    }
    unsigned blocks = 0;
    for (auto* block = pool.m_nodeBlockList; block; block = block->m_next) ++blocks;
    Check(blocks >= 16, "multiple intrusive node blocks exercised");
    std::vector<void*> retrieved;
    for (unsigned i = 0; i < 48; ++i) {
        void* data = nullptr;
        uint32_t bytes = 0;
        pool.GetData(data, bytes, __FILE__, __LINE__);
        Check(data && bytes == 64 && originals.erase(data) == 1, "each payload returned once");
        const auto* contents = static_cast<const unsigned char*>(data);
        for (unsigned j = 1; j < bytes; ++j) Check(contents[j] == contents[0], "payload preserved");
        retrieved.push_back(data);
    }
    Check(originals.empty(), "all cached allocations recovered");
    void* empty = reinterpret_cast<void*>(1);
    uint32_t bytes = 1;
    pool.GetData(empty, bytes, __FILE__, __LINE__);
    Check(!empty && !bytes, "empty pool clears outputs");
    for (void* data : retrieved) pool.PutData(data, 64, __FILE__, __LINE__);
    pool.Clear();
    Check(!pool.m_nodeBlockList && !pool.m_nodeFullList && !pool.m_nodeEmptyList, "quiescent clear");
    pool.Clear();
}
void ConcurrentReuse() {
    CDataRecycler pool(3, 48);
    std::mutex mutex;
    std::set<void*> held;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        for (unsigned i = 0; i < 2000; ++i) {
            void* data = nullptr;
            uint32_t bytes = 0;
            pool.GetData(data, bytes, __FILE__, __LINE__);
            if (!data) data = pool.AllocData(64, &bytes, __FILE__, __LINE__);
            Check(data && bytes == 64, "concurrent allocation/reuse");
            { std::lock_guard<std::mutex> lock(mutex); Check(held.insert(data).second, "exclusive payload ownership"); }
            std::memset(data, static_cast<int>(t + 1), bytes);
            { std::lock_guard<std::mutex> lock(mutex); Check(held.erase(data) == 1, "release tracked ownership"); }
            pool.PutData(data, bytes, __FILE__, __LINE__);
        }
    });
    for (auto& thread : threads) thread.join();
    Check(held.empty(), "all users quiescent before clear");
    pool.Clear();
}
}
int main() {
    MultipleBlocks();
    ConcurrentReuse();
    std::puts("PASS: optimized recycler multiple-block aliasing regression, reuse and quiescent clear");
}
