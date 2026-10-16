#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"

namespace {
using Blizzard::Streaming::LoadRange;
using Blizzard::Streaming::StorageEntry;
namespace Storage = Blizzard::Storage;
namespace Lock = Blizzard::Lock;

void Check(bool condition) {
    if (!condition) {
        std::fputs("StreamingStorageRanges assertion failed\n", stderr);
        std::abort();
    }
}

StorageEntry Entry(int64_t size, int flags = 0) {
    StorageEntry entry{};
    entry.m_size = size;
    entry.m_blockSize = 8;
    entry.m_flags = flags;
    entry.m_blocks.resize((size + 7) / 8);
    return entry;
}

void Equal(const LoadRange& actual, const LoadRange& expected) {
    Check(actual.m_offset == expected.m_offset);
    Check(actual.m_size == expected.m_size);
    Check(actual.m_blockOffset == expected.m_blockOffset);
    Check(actual.m_blockSize == expected.m_blockSize);
    Check(actual.m_state == expected.m_state);
}

void Ranges(StorageEntry& entry, int64_t offset, int size,
            const std::vector<LoadRange>& expected, bool lock = true) {
    int count = -1;
    auto* ranges = Storage::GetRange(&entry, offset, size, &count, lock);
    Check(count == static_cast<int>(expected.size()));
    Check((ranges == nullptr) == expected.empty());
    for (int i = 0; i < count; ++i) Equal(ranges[i], expected[i]);
    if (ranges) Blizzard::Memory::Free(ranges);
}

// Independent per-block oracle for bounded, nonnegative request sizes.
std::vector<LoadRange> Plain(const StorageEntry& entry, int64_t offset, int size) {
    const int states[] = {2, 3, 0, 0, 1};
    int64_t start = offset < 0 ? 0 : offset > entry.m_size ? entry.m_size : offset;
    int64_t end = start + size > entry.m_size ? entry.m_size : start + size;
    std::vector<LoadRange> result;
    while (start < end) {
        const int64_t blockStart = start / 8 * 8;
        const int length = entry.m_size - blockStart < 8 ? entry.m_size - blockStart : 8;
        const int take = end - start < blockStart + length - start ? end - start : blockStart + length - start;
        const int state = entry.m_blocks[start / 8].m_state;
        result.push_back({start, take, state < 2 ? blockStart : 0,
                          state < 2 ? length : 0, states[state]});
        start += take;
    }
    return result;
}

void RangePlanning() {
    for (int64_t size : {0, 1, 8, 19, 24, 37}) {
        auto entry = Entry(size);
        for (int state = 0; state != 5; ++state) {
            for (auto& block : entry.m_blocks) block.m_state = state;
            Check(Storage::IsComplete(&entry) == (size == 0 || state == 3));
            for (int offset = -2; offset <= size + 2; ++offset) {
                for (int length = 0; length <= size + 3; ++length) {
                    Ranges(entry, offset, length, Plain(entry, offset, length), (offset & 1) != 0);
                }
            }
            for (int block = 0; block < static_cast<int>(entry.m_blocks.size()); ++block)
                Check(Storage::GetBlockLength(&entry, block) == (size - block * 8 < 8 ? size - block * 8 : 8));
        }
    }
    auto mixed = Entry(37);
    for (int i = 0; i < 5; ++i) mixed.m_blocks[i].m_state = i;
    Ranges(mixed, 3, 34, {{3,5,0,8,2}, {8,8,8,8,3}, {16,8,0,0,0}, {24,8,0,0,0}, {32,5,0,0,1}});
    Check(Storage::CountRanges(&mixed, 3, 34) == 8);
    Check(Storage::CountRanges(&mixed, 8, 8) == 5);

    auto store = Entry(29, 1);
    store.m_storeSize = 3;
    Ranges(store, 18, 9, {{3,5,0,8,4}, {8,8,8,8,4}, {18,6,16,8,2}, {24,3,24,5,2}});
    store.m_storeSize = 17; // Same block: no prefix range.
    Ranges(store, 18, 9, {{18,6,16,8,2}, {24,3,24,5,2}});
    store.m_storeSize = 25; // Store extends beyond requested offset.
    Ranges(store, 18, 9, {{18,6,16,8,2}, {24,3,24,5,2}});
    store.m_storeSize = 8;
    Ranges(store, 16, 8, {{8,8,8,8,4}, {16,8,16,8,2}});
    Ranges(store, 29, 8, {});

    auto header = Entry(29, 2);
    Ranges(header, 10, 3, {{0,0,0,8,4}, {10,3,8,8,2}, {0,0,16,8,4}, {0,0,24,5,4}});
    for (int state : {0,1,4}) {
        header.m_blocks[0].m_state = state;
        Ranges(header, 10, 3, {{0,0,0,8,4}, {10,3,8,8,2}, {0,0,16,8,4}, {0,0,24,5,4}});
    }
    for (int state : {2,3}) {
        header.m_blocks[0].m_state = state;
        Ranges(header, 10, 3, {{10,3,8,8,2}});
    }
    header = Entry(24, 2);
    // Existing algorithm deliberately includes a zero-length tail at exact EOF.
    Ranges(header, 0, 24, {{0,8,0,8,2}, {8,8,8,8,2}, {16,8,16,8,2}, {0,0,24,0,4}});
    Ranges(header, 0, 0, {});
}

void StateTransitions() {
    for (int requested = 0; requested < 5; ++requested) {
        auto states = Entry(37);
        for (int i = 0; i < 5; ++i) states.m_blocks[i].m_state = i;
        Check(Storage::RequestRange(&states, 7, 30, requested));
        for (int i = 0; i < 5; ++i)
            Check(states.m_blocks[i].m_state == (requested == 1 && (i == 2 || i == 3) ? i : requested));
        Storage::SetRangeAs(&states, 8, 16, requested);
        Check(states.m_blocks[1].m_state == requested && states.m_blocks[2].m_state == requested);
    }
    auto entry = Entry(37);
    for (int i = 0; i < 5; ++i) entry.m_blocks[i].m_state = i;
    Lock::MutexEnter(Storage::s_storageLock); // Exercise recursive calls, not just uncontended lock leaves.
    Check(Storage::RequestRange(&entry, 0, 37, 1));
    const int expected[] = {1,1,2,3,1};
    for (int i = 0; i < 5; ++i) Check(entry.m_blocks[i].m_state == expected[i]);
    Ranges(entry, 0, 8, {{0,8,0,8,3}});
    Storage::SetRangeAs(&entry, 7, 3, 4);
    Check(entry.m_blocks[0].m_state == 4 && entry.m_blocks[1].m_state == 4);
    Check(entry.m_blocks[2].m_state == 2 && entry.m_blocks[3].m_state == 3 && entry.m_blocks[4].m_state == 1);
    Storage::SetRangeAs(&entry, 16, 0, 3); // Existing zero-size semantics touch the containing block.
    Check(entry.m_blocks[2].m_state == 3);
    Check(!Storage::IsComplete(&entry));
    Check(Storage::RequestRange(&entry, 0, 37, 3));
    Check(Storage::IsComplete(&entry));
    entry.m_closed = true;
    Check(!Storage::RequestRange(&entry, 0, 37, 0));
    Check(Storage::IsComplete(&entry));
    Lock::MutexLeave(Storage::s_storageLock);

    auto concurrent = Entry(37);
    auto worker = [&] {
        for (int i = 0; i < 200; ++i) {
            Check(Storage::RequestRange(&concurrent, 0, 37, 1));
            Storage::SetRangeAs(&concurrent, 0, 37, 3);
            int count = 0;
            auto* ranges = Storage::GetRange(&concurrent, 1, 36, &count, true);
            Check(count == 5);
            Blizzard::Memory::Free(ranges);
        }
    };
    std::thread first(worker), second(worker);
    first.join(); second.join();
    Check(Storage::IsComplete(&concurrent));
}
}

int main() {
    Lock::MutexCreate(Storage::s_storageLock);
    RangePlanning();
    StateTransitions();
    Lock::MutexDestroy(Storage::s_storageLock);
    std::puts("StreamingStorageRanges: passed");
}
