// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/WindowsIncludes.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Storage.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/File/Filestack.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

// Native builds reuse only range planning and state transitions here; persistence,
// worker lifecycle and initialization still require their real platform providers.

// STATIC: 0x00B325C0
Blizzard::Lock::Mutex Blizzard::Storage::s_storageLock;

// OFFSET: 0x00427F50
int32_t Blizzard::Storage::GetBlockLength(Blizzard::Streaming::StorageEntry* entry, int32_t block) {
    int32_t blockSize = entry->m_blockSize;

    if (entry->m_size <= (block + 1LL) * blockSize) {
        return static_cast<int32_t>(entry->m_size - static_cast<int64_t>(block) * blockSize);
    }

    return entry->m_blockSize;
}

// OFFSET: 0x00427FB0
int32_t Blizzard::Storage::CountRanges(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int64_t size) {
    return static_cast<int32_t>((size + offset) / entry->m_blockSize - offset / entry->m_blockSize + 4);
}

// OFFSET: 0x0042D820
bool Blizzard::Storage::IsComplete(Blizzard::Streaming::StorageEntry* entry) {
    for (int32_t i = 0; i < static_cast<int32_t>(entry->m_blocks.size()); i++) {
        if (entry->m_blocks[i].m_state != 3) {
            return false;
        }
    }

    return true;
}

// OFFSET: 0x0042D880
int32_t Blizzard::Storage::BuildRanges(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, Blizzard::Streaming::LoadRange* ranges) {
    int64_t position = offset;
    uint32_t block = static_cast<uint32_t>(offset / entry->m_blockSize);
    int32_t count = 0;

    for (int32_t phase = 1; size && phase <= 3; phase++) {
        if (phase == 2) {
            int32_t full = entry->m_blockSize * (size / entry->m_blockSize);

            while (full >= entry->m_blockSize) {
                int32_t state = entry->m_blocks[block].m_state;
                Blizzard::Streaming::LoadRange& range = ranges[count];

                switch (state) {
                    case 0:
                    case 1:
                        count++;
                        range.m_offset = position;
                        range.m_blockOffset = position;
                        range.m_size = entry->m_blockSize;
                        range.m_blockSize = entry->m_blockSize;
                        range.m_state = 3 - (state != 1);
                        break;

                    case 2:
                    case 3:
                    case 4:
                        count++;
                        range.m_size = entry->m_blockSize;
                        range.m_offset = static_cast<int64_t>(block) * entry->m_blockSize;
                        range.m_blockOffset = 0;
                        range.m_blockSize = 0;
                        range.m_state = state == 4;
                        break;
                }

                block++;
                size -= entry->m_blockSize;
                position += entry->m_blockSize;
                full -= entry->m_blockSize;
            }

            continue;
        }

        int64_t blockStart = static_cast<int64_t>(block) * entry->m_blockSize;

        if (phase == 1 && position == blockStart) {
            continue;
        }

        int32_t inner = static_cast<int32_t>(position - blockStart);
        int32_t length = GetBlockLength(entry, block);
        int32_t state = entry->m_blocks[block].m_state;
        int32_t available = length - inner;
        int32_t take = size >= available ? available : size;

        switch (state) {
            case 0:
            case 1: {
                Blizzard::Streaming::LoadRange& range = ranges[count++];
                size -= take;
                range.m_offset = position;
                range.m_blockOffset = blockStart;
                range.m_size = take;
                range.m_blockSize = length;
                range.m_state = 3 - (state != 1);
                break;
            }

            case 2:
            case 3:
            case 4: {
                Blizzard::Streaming::LoadRange& range = ranges[count++];
                range.m_offset = inner + entry->m_blockSize * static_cast<int64_t>(block);
                size -= take;
                range.m_size = take;
                range.m_blockOffset = 0;
                range.m_blockSize = 0;
                range.m_state = state == 4;
                break;
            }
        }

        block++;
        position += entry->m_blockSize - inner;
    }

    return count;
}

// OFFSET: 0x0042DB80
int32_t Blizzard::Storage::BuildRangesFromStore(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, Blizzard::Streaming::LoadRange* ranges) {
    int64_t start = entry->m_storeSize >= offset ? offset : entry->m_storeSize;
    int32_t count = 0;

    if (start / entry->m_blockSize < offset / entry->m_blockSize) {
        count = BuildRanges(entry, entry->m_storeSize, static_cast<int32_t>(offset / entry->m_blockSize * entry->m_blockSize - entry->m_storeSize), ranges);

        for (int32_t i = 0; i < count; i++) {
            ranges[i].m_state = 4;
        }
    }

    return count + BuildRanges(entry, offset, size, ranges + count);
}

// OFFSET: 0x0042DC70
int32_t Blizzard::Storage::BuildRangesWithHeader(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, Blizzard::Streaming::LoadRange* ranges) {
    if (entry->m_blocks[0].m_state == 3) {
        return BuildRanges(entry, offset, size, ranges);
    }

    if (entry->m_blocks[0].m_state == 2) {
        return BuildRanges(entry, offset, size, ranges);
    }

    int32_t total = static_cast<int32_t>(entry->m_size / entry->m_blockSize + 1);
    int32_t first = static_cast<int32_t>(offset / entry->m_blockSize);
    int32_t head = 0;

    if (first > 0) {
        head = first;

        for (int32_t i = 0; i < first; i++) {
            Blizzard::Streaming::LoadRange& range = ranges[i];
            int32_t length = GetBlockLength(entry, i);
            range.m_offset = 0;
            range.m_size = 0;
            range.m_blockOffset = static_cast<int64_t>(i) * entry->m_blockSize;
            range.m_blockSize = length;
            range.m_state = 4;
        }
    }

    int32_t count = BuildRanges(entry, offset, size, ranges + head) + head;
    int32_t next = static_cast<int32_t>((offset + size - 1) / entry->m_blockSize + 1);

    if (next < total) {
        Blizzard::Streaming::LoadRange* range = &ranges[count];
        count += total - next;

        for (; next < total; next++, range++) {
            int64_t end = (next + 1LL) * entry->m_blockSize;
            int32_t length = entry->m_size <= end ? static_cast<int32_t>(entry->m_size - static_cast<int64_t>(next) * entry->m_blockSize) : entry->m_blockSize;

            range->m_offset = 0;
            range->m_size = 0;
            range->m_blockOffset = static_cast<int64_t>(next) * entry->m_blockSize;
            range->m_blockSize = length;
            range->m_state = 4;
        }
    }

    return count;
}

// OFFSET: 0x0042DE70
bool Blizzard::Storage::RequestRange(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, int32_t state) {

    Blizzard::Lock::MutexEnter(s_storageLock);

    if (entry->m_closed) {

        Blizzard::Lock::MutexLeave(s_storageLock);

        return false;
    }

    int64_t blockSize = entry->m_blockSize;
    int32_t last = static_cast<int32_t>((offset + size - 1) / blockSize);

    for (uint32_t block = static_cast<uint32_t>(offset / blockSize); static_cast<int32_t>(block) <= last; block++) {
        Blizzard::Streaming::StorageBlock& storageBlock = entry->m_blocks[block];

        if (state != 1 || (storageBlock.m_state != 2 && storageBlock.m_state != 3)) {
            storageBlock.m_state = state;
        }
    }

    Blizzard::Lock::MutexLeave(s_storageLock);

    return true;
}

// OFFSET: 0x0042DF70
void Blizzard::Storage::SetRangeAs(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, int32_t state) {
    int32_t first = static_cast<int32_t>(offset / entry->m_blockSize);
    int32_t last = size <= 0 ? first : static_cast<int32_t>((offset + size - 1) / entry->m_blockSize);

    Blizzard::Lock::MutexEnter(s_storageLock);

    for (uint32_t block = first; static_cast<int32_t>(block) <= last; block++) {
        entry->m_blocks[block].m_state = state;
    }

    Blizzard::Lock::MutexLeave(s_storageLock);

}

// OFFSET: 0x00432A40
Blizzard::Streaming::LoadRange* Blizzard::Storage::GetRange(Blizzard::Streaming::StorageEntry* entry, int64_t offset, int32_t size, int32_t* count, bool lock) {
    int64_t zero = 0;
    int64_t start = offset >= 0 ? offset : zero;
    start = entry->m_size >= start ? start : entry->m_size;

    int64_t end = start + size;
    zero = 0;
    end = end >= 0 ? end : zero;
    end = entry->m_size >= end ? end : entry->m_size;

    int32_t length = static_cast<int32_t>(end - start);

    if (end == start) {
        *count = 0;
        return nullptr;
    }

    if (lock) {

        Blizzard::Lock::MutexEnter(s_storageLock);

    }

    Blizzard::Streaming::LoadRange* ranges;

    if (entry->m_flags & 2) {
        ranges = static_cast<Blizzard::Streaming::LoadRange*>(Blizzard::Memory::Allocate(sizeof(Blizzard::Streaming::LoadRange) * CountRanges(entry, 0, entry->m_size)));
        *count = BuildRangesWithHeader(entry, start, length, ranges);
    } else if (entry->m_flags & 1) {
        zero = 0;
        int64_t from = entry->m_storeSize >= 0 ? entry->m_storeSize : zero;
        from = start >= from ? from : start;
        ranges = static_cast<Blizzard::Streaming::LoadRange*>(Blizzard::Memory::Allocate(sizeof(Blizzard::Streaming::LoadRange) * CountRanges(entry, from, start + length - from)));
        *count = BuildRangesFromStore(entry, start, length, ranges);
    } else {
        ranges = static_cast<Blizzard::Streaming::LoadRange*>(Blizzard::Memory::Allocate(sizeof(Blizzard::Streaming::LoadRange) * CountRanges(entry, start, length)));
        *count = BuildRanges(entry, start, length, ranges);
    }

    if (lock) {

        Blizzard::Lock::MutexLeave(s_storageLock);

    }

    return ranges;
}
