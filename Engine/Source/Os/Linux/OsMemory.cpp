#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include "Engine/Source/Os/OsMemory.h"

namespace {
constexpr uintptr_t NearRange = 0x60000000;
struct NearAllocations {
    std::mutex mutex;
    std::unordered_map<void*, size_t> sizes;
};
NearAllocations& Allocations() {
    static NearAllocations allocations;
    return allocations;
}
void* TryMapping(uintptr_t candidate, size_t bytes, uintptr_t minimum, uintptr_t maximum) {
    // Hints may be ignored or raced. Never use MAP_FIXED, and verify the actual
    // returned interval before publishing it. Existing mappings cannot be lost.
    void* result = mmap(reinterpret_cast<void*>(candidate), bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) return nullptr;
    const uintptr_t address = reinterpret_cast<uintptr_t>(result);
    if (address < minimum || address > maximum || bytes > maximum - address) {
        munmap(result, bytes);
        return nullptr;
    }
    return result;
}
void* Track(void* result, size_t bytes) {
    if (!result) return nullptr;
    try {
        auto& allocations = Allocations();
        std::lock_guard<std::mutex> lock(allocations.mutex);
        allocations.sizes.emplace(result, bytes);
        return result;
    } catch (const std::bad_alloc&) {
        munmap(result, bytes);
        errno = ENOMEM;
        return nullptr;
    }
}
}

void* OsAllocNear(const void* anchor, size_t size) {
    if (!size) { errno = EINVAL; return nullptr; }
    const long pageQuery = sysconf(_SC_PAGESIZE);
    if (pageQuery <= 0) { errno = EINVAL; return nullptr; }
    const uintptr_t page = static_cast<uintptr_t>(pageQuery);
    constexpr uintptr_t limit = std::numeric_limits<uintptr_t>::max();
    if (size > limit - (page - 1)) { errno = EOVERFLOW; return nullptr; }
    const size_t bytes = (size + page - 1) / page * page;
    const uintptr_t center = reinterpret_cast<uintptr_t>(anchor);
    uintptr_t minimum = center > NearRange ? center - NearRange : page;
    const uintptr_t maximum = center > limit - NearRange ? limit : center + NearRange;
    if (minimum > limit - (page - 1)) { errno = ENOMEM; return nullptr; }
    minimum = (minimum + page - 1) / page * page;
    if (maximum < minimum || bytes > maximum - minimum) { errno = ENOMEM; return nullptr; }

    if (void* result = TryMapping(center / page * page, bytes, minimum, maximum)) {
        return Track(result, bytes);
    }
    // A snapshot supplies free-gap hints, not permission to overwrite anything.
    // Concurrent mappings remain safe because every mmap is non-fixed.
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return nullptr;
    uintptr_t previous = minimum;
    char line[4096];
    void* result = nullptr;
    while (std::fgets(line, sizeof(line), maps)) {
        uintptr_t start, end;
        if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) != 2) continue;
        if (end <= previous) continue;
        const uintptr_t gapEnd = start < maximum ? start : maximum;
        if (gapEnd > previous && bytes <= gapEnd - previous) {
            result = TryMapping(previous, bytes, minimum, maximum);
            if (result) break;
        }
        if (end >= maximum) { previous = maximum; break; }
        previous = end;
    }
    if (!result && previous < maximum && bytes <= maximum - previous) {
        result = TryMapping(previous, bytes, minimum, maximum);
    }
    std::fclose(maps);
    if (!result) { errno = ENOMEM; return nullptr; }
    return Track(result, bytes);
}

void OsFreeNear(void* ptr) {
    if (!ptr) return;
    auto& allocations = Allocations();
    std::lock_guard<std::mutex> lock(allocations.mutex);
    const auto found = allocations.sizes.find(ptr);
    if (found == allocations.sizes.end()) { errno = EINVAL; return; }
    if (munmap(ptr, found->second) == 0) allocations.sizes.erase(found);
}
