#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <new>
#include "Engine/Source/Storm/H/storm.h"

namespace {
std::atomic<bool> s_exitOnFailure{true};
std::atomic<bool> s_heapTracking{true};

void Failure(size_t bytes, const char* filename, int32_t line) {
    char text[128];
    std::snprintf(text, sizeof(text), "Requested %zu bytes of memory", bytes);
    SErrSetLastError(STORM_ERROR_NOT_ENOUGH_MEMORY);
    const bool fatal = s_exitOnFailure.load();
    SErrDisplayError(STORM_ERROR_NOT_ENOUGH_MEMORY, filename, line, text, !fatal, 1, 0x11111111);
    if (fatal) std::_Exit(1);
}

bool RoundedSize(size_t bytes, size_t& size) {
    if (bytes > std::numeric_limits<size_t>::max() - 7) return false;
    size = (bytes + 7) & ~size_t{7};
    return true;
}

void ClearSlack(void* pointer, size_t size) {
    // glibc reports allocator slack as usable size. Keep it zero so realloc's
    // ZERO flag also covers growth into an existing chunk, without changing
    // pointer ownership or adding headers incompatible with malloc/free.
    const size_t usable = malloc_usable_size(pointer);
    if (usable > size) std::memset(static_cast<unsigned char*>(pointer) + size, 0, usable - size);
}
}

void STORMAPI SMemSetDebugFlags(uint32_t flags, uint8_t mask) {
    if (mask & SMEM_DEBUG_NOEXITONFAILURE) s_exitOnFailure = !(flags & SMEM_DEBUG_NOEXITONFAILURE);
    if (mask & SMEM_DEBUG_HEAPTRACKING) s_heapTracking = (flags & SMEM_DEBUG_HEAPTRACKING) != 0;
}

void* STORMAPI SMemAlloc(size_t bytes, const char* filename, int32_t line, uint32_t flags) {
    size_t size;
    if (!RoundedSize(bytes, size)) { Failure(bytes, filename, line); return nullptr; }
    void* pointer = flags & SMEM_FLAG_ZEROMEMORY ? std::calloc(1, size) : std::malloc(size);
    if (!pointer) { Failure(bytes, filename, line); return nullptr; }
    ClearSlack(pointer, size);
    return pointer;
}

int32_t STORMAPI SMemFree(void* pointer, const char*, int32_t, uint32_t) {
    std::free(pointer);
    return 1;
}

void* STORMAPI SMemReAlloc(void* pointer, size_t bytes, const char* filename, int32_t line, uint32_t flags) {
    if (flags == SMEM_REALLOC_NULL) return nullptr;
    if (!pointer) return SMemAlloc(bytes, filename, line, flags);
    if (flags & SMEM_FLAG_NOREALLOC) return nullptr;
    size_t size;
    if (!RoundedSize(bytes, size)) { Failure(bytes, filename, line); return nullptr; }
    if (!size) { std::free(pointer); return nullptr; }
    const size_t oldSize = malloc_usable_size(pointer);
    const uintptr_t oldAddress = reinterpret_cast<uintptr_t>(pointer);
    void* replacement = std::realloc(pointer, size);
    if (!replacement) { Failure(bytes, filename, line); return nullptr; }
    if ((flags & SMEM_FLAG_ZEROMEMORY) && oldSize < bytes) {
        std::memset(static_cast<unsigned char*>(replacement) + oldSize, 0, bytes - oldSize);
    }
    ClearSlack(replacement, size);
    if (s_heapTracking.load() && reinterpret_cast<uintptr_t>(replacement) != oldAddress) {
        DebugLog(-1, "REALLOC %zu bytes at %p from address %zx (%s:%d)", bytes,
            replacement, static_cast<size_t>(oldAddress), filename ? filename : "", line);
    }
    return replacement;
}

void* operator new(size_t bytes) {
    if (void* pointer = SMemAlloc(bytes ? bytes : 1, "new", SLOG_FUNCTION, 0)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes); }
    catch (const std::bad_alloc&) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }
