#ifndef ENGINE_SOURCE_OS_POSIX_PATH_H
#define ENGINE_SOURCE_OS_POSIX_PATH_H

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>

inline std::filesystem::path OsPosixPath(const char* input) {
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');
    return std::filesystem::path(path);
}

// These APIs expose the legacy client error domain, not POSIX errno values.
inline uint32_t OsPosixFileError(int error) {
    switch (error) {
        case 0: return 0;
        case ENOENT: return 2;
        case ENOTDIR: return 3;
        case EACCES: case EPERM: return 5;
        case EBADF: return 6;
        case ENOMEM: return 8;
        case EROFS: return 19;
        case EEXIST: return 183;
        case ENOSPC: return 112;
        case ENAMETOOLONG: return 206;
        case EINVAL: return 87;
        default: return 31;
    }
}

#endif
