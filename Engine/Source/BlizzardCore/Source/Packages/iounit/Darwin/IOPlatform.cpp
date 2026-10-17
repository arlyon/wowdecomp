#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOPlatform.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"

namespace {
bool Failure(int error) { errno = error; Blizzard::File::SetLastError(8); return false; }
std::string Path(const char* name) {
    std::string path(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}
struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        const int saved = errno;
        if (value >= 0) ::close(value);
        errno = saved;
    }
};

bool Alignment(int fd, uint32_t& alignment) {
    struct stat info{};
    if (fstat(fd, &info) != 0) return Failure(errno);
    if (info.st_blksize <= 0 || static_cast<uint64_t>(info.st_blksize) > UINT32_MAX) return Failure(EOVERFLOW);
    uint32_t bytes = std::max<uint32_t>(static_cast<uint32_t>(info.st_blksize), sizeof(void*));
#ifdef STATX_DIOALIGN
    struct statx extended{};
    if (statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &extended) == 0 && (extended.stx_mask & STATX_DIOALIGN)) {
        bytes = std::max({bytes, extended.stx_dio_mem_align, extended.stx_dio_offset_align});
    }
#endif
    if (bytes & (bytes - 1)) return Failure(ENOTSUP);
    alignment = bytes;
    return true;
}
}

bool System_File::Native::GetFileAlignment(const char* path, uint32_t& alignment) {
    if (!path || !*path) return Failure(EINVAL);
    const std::string native = Path(path);
    int fd;
    do { fd = open(native.c_str(), O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    Descriptor file(fd);
    if (fd < 0) return Failure(errno);
    return Alignment(fd, alignment);
}

bool System_File::Native::ReadUnbuffered(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    if (bytesRead) *bytesRead = 0;
    if (!path || !*path || (count && !buffer)) return Failure(EINVAL);
    const std::string native = Path(path);
    int fd;
    do { fd = open(native.c_str(), O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    Descriptor file(fd);
    if (fd < 0) return Failure(errno);
    if (fcntl(fd, F_NOCACHE, 1) != 0) return Failure(errno);
    if (!count) return true;
    uint32_t alignment;
    if (!Alignment(fd, alignment)) return false;
    const uint64_t mask = alignment - 1;
    const uint64_t head = offset & mask;
    const uint64_t size = (head + count + mask) & ~mask;
    if (size > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())) return Failure(EOVERFLOW);
    void* allocation = nullptr;
    const int allocated = posix_memalign(&allocation, alignment, static_cast<size_t>(size));
    if (allocated) return Failure(allocated);
    std::unique_ptr<void, decltype(&std::free)> data(allocation, &std::free);
    ssize_t received;
    do { received = pread(fd, data.get(), static_cast<size_t>(size), offset - head); } while (received < 0 && errno == EINTR);
    if (received < 0) return Failure(errno);
    // A read that ends before the requested prefix is ordinary EOF. Avoid the
    // unsigned underflow present in a naive received-minus-head calculation.
    const uint64_t available = static_cast<uint64_t>(received) > head ? received - head : 0;
    const uint32_t amount = static_cast<uint32_t>(std::min<uint64_t>(count, available));
    if (amount) std::memcpy(buffer, static_cast<uint8_t*>(data.get()) + head, amount);
    if (bytesRead) *bytesRead = amount;
    return true;
}
