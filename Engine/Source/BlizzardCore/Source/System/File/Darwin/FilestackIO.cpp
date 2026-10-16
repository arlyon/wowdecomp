#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
using Stream = Blizzard::File::StreamRecord;
using Info = Blizzard::File::FileInfo;
using Parms = System_File::Stacked::FileParms;

bool Failure(int error, int code = 8) {
    errno = error;
    Blizzard::File::SetLastError(code);
    return false;
}

std::string Path(const char* name) {
    std::string path(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

int Descriptor(const Stream* stream) {
    if (!stream || !stream->m_fileHandle) { errno = EBADF; return -1; }
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(stream->m_fileHandle);
    if (encoded > static_cast<uintptr_t>(INT32_MAX) + 1) { errno = EBADF; return -1; }
    return static_cast<int>(encoded - 1);
}

bool Timestamp(const timespec& time, int64_t& result) {
    // Blizzard timestamps are nanoseconds since 2000-01-01, not Unix time.
    constexpr int64_t epoch = 946684800;
    const __int128 value = (static_cast<__int128>(time.tv_sec) - epoch) * 1000000000 + time.tv_nsec;
    if (value < INT64_MIN || value > INT64_MAX) return Failure(EOVERFLOW);
    result = static_cast<int64_t>(value);
    return true;
}

bool FillInfo(const struct stat& status, const char* path, Info& info) {
    info = {};
    info.m_size = status.st_size;
    info.m_attributes = S_ISDIR(status.st_mode) ? 0x40 : 0x20;
    if (!(status.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) info.m_attributes |= 1;
    info.m_regularFile = S_ISREG(status.st_mode);
    info.m_existence = S_ISDIR(status.st_mode) ? 2 : 1;
    if (!Timestamp(status.st_mtimespec, info.m_modTime) || !Timestamp(status.st_atimespec, info.m_accessTime)) return false;
    if (!Timestamp(status.st_birthtimespec, info.m_createTime)) return false;
    // Preserve the Linux reference's optional statx handling below.
#ifdef STATX_BTIME
    struct statx extended{};
    if (path && statx(AT_FDCWD, path, 0, STATX_BTIME, &extended) == 0 && (extended.stx_mask & STATX_BTIME)) {
        timespec birth{static_cast<time_t>(extended.stx_btime.tv_sec), static_cast<long>(extended.stx_btime.tv_nsec)};
        if (!Timestamp(birth, info.m_createTime)) return false;
    }
#else
    (void)path;
#endif
    return true;
}

bool Refresh(Stream& stream) {
    struct stat status{};
    const int fd = Descriptor(&stream);
    stream.m_infoValid = false;
    if (fd < 0 || fstat(fd, &status) != 0) return Failure(errno);
    Info info;
    if (!FillInfo(status, stream.m_pathname, info)) return false;
    if (stream.m_streaming && stream.m_streaming->m_entry) info.m_size = stream.m_info.m_size;
    info.m_path = stream.m_pathname;
    stream.m_info = info;
    stream.m_infoValid = true;
    return true;
}

int NativeOpenFlags(uint32_t mode) {
    int flags = O_CLOEXEC;
    switch (mode & 3) {
        case 0:
            // O_PATH cannot create files; metadata-only handles still need
            // a real open when the legacy disposition requests creation.
            flags |= O_RDONLY;
            break;
        case 1: flags |= O_RDONLY; break;
        case 2: flags |= O_WRONLY; break;
        case 3: flags |= O_RDWR; break;
    }
    if ((mode & 0x100) || (mode & 0x400)) flags |= O_CREAT | ((mode & 0x800) ? O_EXCL : O_TRUNC);
    else if (mode & 0x800) flags |= O_CREAT | O_EXCL;
    else if (!(mode & 0x1000)) flags |= O_CREAT;
    return flags;
}

bool Transfer(Parms& parms, bool write, bool positioned) {
    const int fd = Descriptor(parms.m_stream);
    if (fd < 0) return Failure(errno);
    if (!(parms.m_stream->m_mode & (write ? 2 : 1))) return Failure(EBADF);
    if (parms.m_amount < 0 || (parms.m_amount && !parms.m_buffer) || (positioned && parms.m_pos < 0)) return Failure(EINVAL);
    ssize_t amount;
    do {
        if (write) amount = positioned ? pwrite(fd, parms.m_buffer, static_cast<size_t>(parms.m_amount), parms.m_pos) :
                                         ::write(fd, parms.m_buffer, static_cast<size_t>(parms.m_amount));
        else amount = positioned ? pread(fd, parms.m_buffer, static_cast<size_t>(parms.m_amount), parms.m_pos) :
                                   ::read(fd, parms.m_buffer, static_cast<size_t>(parms.m_amount));
    } while (amount < 0 && errno == EINTR);
    if (amount < 0) return Failure(errno);
    parms.m_amount = static_cast<int32_t>(amount);
    if (write) {
        parms.m_stream->m_infoValid = false;
        if ((parms.m_stream->m_mode & 0x40) && fsync(fd) != 0) return Failure(errno);
    }
    return true;
}

int Whence(int32_t value) { return value == 0 ? SEEK_SET : value == 1 ? SEEK_CUR : SEEK_END; }
}

bool System_File::Open(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms || !parms->m_path || !*parms->m_path) return Failure(EINVAL, 4);
    const std::string path = Path(parms->m_path);
    int fd;
    do { fd = open(path.c_str(), NativeOpenFlags(parms->m_mode), 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return Failure(errno, 4);
    Stream* stream = parms->m_stream;
    if (!stream) {
        const size_t bytes = sizeof(Stream) + path.size() + 1;
        stream = static_cast<Stream*>(SMemAlloc(bytes, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        if (!stream) { ::close(fd); return Failure(ENOMEM, 4); }
        stream->m_pathname = reinterpret_cast<char*>(stream + 1);
        std::memcpy(stream->m_pathname, path.c_str(), path.size() + 1);
    }
    if ((parms->m_mode & 0x40) && fcntl(fd, F_NOCACHE, 1) != 0) {
        const int error = errno;
        ::close(fd);
        if (!parms->m_stream) SMemFree(stream, __FILE__, __LINE__, 0);
        return Failure(error, 4);
    }
    stream->m_fileHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(fd) + 1);
    stream->m_mode = parms->m_mode;
    stream->m_infoValid = false;
    if (!Refresh(*stream) || ((parms->m_mode & 0x200) && lseek(fd, 0, SEEK_END) < 0)) {
        const int error = errno;
        ::close(fd);
        stream->m_fileHandle = nullptr;
        if (!parms->m_stream) SMemFree(stream, __FILE__, __LINE__, 0);
        return Failure(error, 4);
    }
    parms->m_stream = stream;
    return true;
}

bool System_File::Close(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    if (fd < 0) return Failure(errno);
    // On Linux close releases the descriptor even when interrupted; retrying
    // could close a different thread's newly reused descriptor.
    const int result = ::close(fd);
    const int error = errno;
    SMemFree(parms->m_stream, __FILE__, __LINE__, 0);
    parms->m_stream = nullptr;
    return result == 0 || Failure(error);
}

bool System_File::Read(Blizzard::File::Filesystem*, Parms* parms) { return parms ? Transfer(*parms, false, false) : Failure(EINVAL); }
bool System_File::ReadP(Blizzard::File::Filesystem*, Parms* parms) { return parms ? Transfer(*parms, false, true) : Failure(EINVAL); }
bool System_File::Write(Blizzard::File::Filesystem*, Parms* parms) { return parms ? Transfer(*parms, true, false) : Failure(EINVAL); }
bool System_File::WriteP(Blizzard::File::Filesystem*, Parms* parms) { return parms ? Transfer(*parms, true, true) : Failure(EINVAL); }

bool System_File::GetPos(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    if (fd < 0) return Failure(errno);
    const off_t offset = lseek(fd, 0, SEEK_CUR);
    if (offset < 0) return Failure(errno);
    parms->m_pos = offset;
    return true;
}

bool System_File::SetPos(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    if (fd < 0 || lseek(fd, parms->m_pos, Whence(parms->m_posMode)) < 0) return Failure(errno);
    return true;
}

bool System_File::SetEOF(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    if (fd < 0) return Failure(errno);
    const off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0) return Failure(errno);
    const off_t end = lseek(fd, parms->m_pos, Whence(parms->m_posMode));
    if (end < 0) return Failure(errno);
    const int result = ftruncate(fd, end);
    const int error = errno;
    const bool restored = lseek(fd, saved, SEEK_SET) >= 0;
    parms->m_stream->m_infoValid = false;
    if (!result && restored) return true;
    return Failure(result ? error : errno);
}

bool System_File::Flush(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    return fd >= 0 && fsync(fd) == 0 ? true : Failure(errno);
}

void System_File::GetFileInfoByFile(Stream* stream) {
    if (!stream) { Failure(EINVAL); return; }
    if (!stream->m_infoValid) Refresh(*stream);
}

bool System_File::GetFileInfo(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    if (parms->m_path) {
        if (!parms->m_info) return Failure(EINVAL);
        const std::string path = Path(parms->m_path);
        struct stat status{};
        if (stat(path.c_str(), &status) != 0) return Failure(errno);
        return FillInfo(status, path.c_str(), *parms->m_info);
    }
    if (!parms->m_stream) return Failure(EINVAL);
    GetFileInfoByFile(parms->m_stream);
    parms->m_info = &parms->m_stream->m_info;
    return parms->m_stream->m_infoValid;
}

bool System_File::Exists(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms || !parms->m_path || !parms->m_info) return Failure(EINVAL);
    struct stat status{};
    if (stat(Path(parms->m_path).c_str(), &status) == 0) {
        parms->m_info->m_existence = S_ISDIR(status.st_mode) ? 2 : 1;
        return true;
    }
    parms->m_info->m_existence = 0;
    return errno == ENOENT || errno == ENOTDIR ? true : Failure(errno);
}

bool System_File::SetCacheMode(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    const int fd = Descriptor(parms->m_stream);
    if (fd < 0) return Failure(errno);
    if (fcntl(fd, F_NOCACHE, (parms->m_mode & 0x40) != 0) != 0) return Failure(errno);
    parms->m_stream->m_mode = (parms->m_stream->m_mode & ~0x40) | (parms->m_mode & 0x40);
    return true;
}

bool System_File::SetAttributes(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms || !parms->m_info) return Failure(EINVAL);
    if (parms->m_has.m_all & ~uint32_t{0x14}) return Failure(ENOTSUP);
    if (parms->m_has.m_all & 0x10) {
        const int fd = Descriptor(parms->m_stream);
        if (fd < 0) return Failure(errno);
        const int64_t time = parms->m_info->m_modTime;
        int64_t seconds = time / 1000000000;
        int64_t nanos = time % 1000000000;
        if (nanos < 0) { --seconds; nanos += 1000000000; }
        timespec times[] = {{0, UTIME_OMIT}, {seconds + 946684800, static_cast<long>(nanos)}};
        if (futimens(fd, times) != 0) return Failure(errno);
        parms->m_stream->m_infoValid = false;
        parms->m_has.m_all &= ~uint32_t{0x10};
    }
    if (parms->m_has.m_all & 4) {
        if (!parms->m_path || !*parms->m_path) return Failure(EINVAL);
        const int32_t attributes = parms->m_info->m_attributes;
        // File/directory type is intrinsic; POSIX has no equivalent for the
        // Windows hidden/system/archive/temporary attribute switches.
        if (attributes & ~int32_t{0x61}) return Failure(ENOTSUP);
        const std::string path = Path(parms->m_path);
        struct stat status{};
        if (stat(path.c_str(), &status) != 0) return Failure(errno);
        const mode_t mode = attributes & 1 ? status.st_mode & ~mode_t{0222} : status.st_mode | S_IWUSR;
        if (chmod(path.c_str(), mode & 07777) != 0) return Failure(errno);
        parms->m_has.m_all &= ~uint32_t{4};
    }
    return true;
}
