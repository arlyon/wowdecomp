#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/Posix/Path.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <vector>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
int Fail(int error) {
    errno = error;
    SErrSetLastError(error == ENOTSUP ? 50 : OsPosixFileError(error));
    return 0;
}
bool Path(const char* name, std::string& path) {
    if (!name || !*name) { Fail(EINVAL); return false; }
    path = OsPosixPath(name).string();
    return true;
}
struct Handle { int fd; dev_t device; ino_t inode; uint32_t access, share; };
std::mutex handlesMutex;
std::map<uintptr_t, Handle> handles;
uintptr_t nextHandle = 1;
bool Same(const Handle& handle, const struct stat& st) {
    return handle.device == st.st_dev && handle.inode == st.st_ino;
}
// Sharing is enforced among these handles only, not against other processes or
// POSIX callers. Tokens never recycle, even when the kernel reuses a descriptor.
bool Shared(const struct stat& st, uint32_t access, uint32_t share) {
    for (const auto& entry : handles) {
        const auto& h = entry.second;
        if (Same(h, st) && ((access & ~h.share) || (h.access & ~share))) {
            Fail(EACCES); SErrSetLastError(32); return false;
        }
    }
    return true;
}
bool Mutable(const struct stat& st) {
    for (const auto& entry : handles) if (Same(entry.second, st)) {
        Fail(EACCES); SErrSetLastError(32); return false;
    }
    return true;
}
uint32_t Attributes(const struct stat& st, const char* name) {
    uint32_t result = S_ISDIR(st.st_mode) ? 0x40 : 0x20;
    if (!(st.st_mode & 0222)) result |= 1;
    const char* base = std::strrchr(name, '/');
    if ((base ? base[1] : name[0]) == '.') result |= 2;
    return result;
}
bool Glob(const char* name, const char* pattern) {
    const char* star = nullptr; const char* retry = nullptr;
    while (*name) {
        if (*pattern == '*') { star = ++pattern; retry = name; continue; }
        const char* p = pattern;
        if (*p == '\\' && p[1]) ++p;
        if (*pattern == '?' || (*p && std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*name)))) {
            ++name; pattern = p + 1; continue;
        }
        if (!star) return false;
        pattern = star; name = ++retry;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}
// All descendants are resolved relative to opened directories. Never follow a
// symlink while traversing or chmod a symlink target during recursive deletion.
bool RemoveContents(int fd, bool readonly) {
    const int duplicate = dup(fd);
    if (duplicate < 0) return Fail(errno);
    DIR* dir = fdopendir(duplicate);
    if (!dir) {
        const int error = errno;
        close(duplicate);
        return Fail(error);
    }
    struct stat parentStatus{};
    if (fstat(fd, &parentStatus)) { int error = errno; closedir(dir); return Fail(error); }
    bool ok = true;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) { if (errno) ok = Fail(errno); break; }
        const char* name = entry->d_name;
        if (!std::strcmp(name, ".") || !std::strcmp(name, "..")) continue;
        struct stat st{};
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { ok = Fail(errno); break; }
        if (!Mutable(st)) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != parentStatus.st_dev) { ok = Fail(EXDEV); break; }
            int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ok = Fail(errno); break; }
            ok = RemoveContents(child, readonly);
            close(child);
            if (!ok) break;
            // A replaced directory is never traversed; unlinkat cannot follow it.
            if (unlinkat(fd, name, AT_REMOVEDIR)) { ok = Fail(errno); break; }
        } else {
            if (!S_ISLNK(st.st_mode) && !(st.st_mode & 0222) && !readonly) { ok = Fail(EACCES); break; }
            if (unlinkat(fd, name, 0)) { ok = Fail(errno); break; }
        }
    }
    const int error = errno; closedir(dir); errno = error;
    return ok;
}
}

HOSFILE OsCreateFile(const char* name, uint32_t access, uint32_t share, uint32_t disposition, uint32_t attributes, uint32_t type) {
    std::string path;
    if (!Path(name, path)) return OS_INVALID_FILE;
    if (!access || (access & ~(OS_GENERIC_READ | OS_GENERIC_WRITE)) || disposition < 1 || disposition > 5) { Fail(EINVAL); return OS_INVALID_FILE; }
    if ((share & ~3u) || (attributes & ~0x81u) || (type && type != OS_FILE_TYPE_ANY)) { Fail(ENOTSUP); return OS_INVALID_FILE; }
    if ((disposition == 2 || disposition == 5) && !(access & OS_GENERIC_WRITE)) { Fail(EINVAL); return OS_INVALID_FILE; }
    const uint32_t rights = ((access & OS_GENERIC_READ) ? 1 : 0) | ((access & OS_GENERIC_WRITE) ? 2 : 0);
    int flags = O_CLOEXEC | O_NONBLOCK | (rights == 3 ? O_RDWR : rights == 2 ? O_WRONLY : O_RDONLY);
    if (disposition == 1) flags |= O_CREAT | O_EXCL;
    if (disposition == 2 || disposition == 4) flags |= O_CREAT;
    std::lock_guard<std::mutex> lock(handlesMutex);
    int fd;
    do { fd = open(path.c_str(), flags, 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) { Fail(errno); return OS_INVALID_FILE; }
    struct stat st{};
    auto reject = [&](int error) { close(fd); Fail(error); return OS_INVALID_FILE; };
    if (fstat(fd, &st)) return reject(errno);
    if (!S_ISREG(st.st_mode)) return reject(ENOTSUP);
    if (!Shared(st, rights, share)) { close(fd); return OS_INVALID_FILE; }
    if (nextHandle == std::numeric_limits<uintptr_t>::max()) return reject(EMFILE);
    if ((disposition == 2 || disposition == 5) && ftruncate(fd, 0)) return reject(errno);
    if ((attributes & 1) && fchmod(fd, st.st_mode & ~0222)) return reject(errno);
    const uintptr_t token = nextHandle++;
    handles.emplace(token, Handle{fd, st.st_dev, st.st_ino, rights, share});
    return reinterpret_cast<HOSFILE>(token);
}
bool OsCloseFile(HOSFILE file) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(reinterpret_cast<uintptr_t>(file));
    if (it == handles.end()) return Fail(EBADF);
    int fd = it->second.fd; handles.erase(it);
    // Linux close consumes the descriptor even on EINTR; never retry it.
    return close(fd) == 0 || Fail(errno);
}
namespace {
int Transfer(HOSFILE file, void* buffer, uint32_t amount, uint32_t* transferred, bool writing) {
    if (transferred) *transferred = 0;
    if (!buffer || !transferred || amount > INT32_MAX) return Fail(EINVAL);
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(reinterpret_cast<uintptr_t>(file));
    if (it == handles.end()) return Fail(EBADF);
    if (!(it->second.access & (writing ? 2 : 1))) return Fail(EBADF);
    ssize_t result;
    do { result = writing ? write(it->second.fd, buffer, amount) : read(it->second.fd, buffer, amount); } while (result < 0 && errno == EINTR);
    if (result < 0) return Fail(errno);
    *transferred = static_cast<uint32_t>(result);
    return 1;
}
}
int32_t OsReadFile(HOSFILE file, void* buffer, uint32_t amount, uint32_t* transferred) { return Transfer(file, buffer, amount, transferred, false); }
int32_t OsWriteFile(HOSFILE file, const void* buffer, uint32_t amount, uint32_t* transferred) { return Transfer(file, const_cast<void*>(buffer), amount, transferred, true); }
uint64_t OsGetFileSize(HOSFILE file) {
    std::lock_guard<std::mutex> lock(handlesMutex);
    auto it = handles.find(reinterpret_cast<uintptr_t>(file));
    struct stat st{};
    if (it == handles.end()) { Fail(EBADF); return 0xFFFFFFFF; }
    if (fstat(it->second.fd, &st)) { Fail(errno); return 0xFFFFFFFF; }
    return static_cast<uint64_t>(st.st_size);
}
uint32_t OsGetFileAttributes(const char* name) {
    if (!name) { Fail(EINVAL); return 0; }
    std::string path; struct stat st{};
    if (!Path(name, path)) return UINT32_MAX;
    if (stat(path.c_str(), &st)) { Fail(errno); return UINT32_MAX; }
    return Attributes(st, path.c_str());
}
int32_t OsFileExists(const char* name) {
    std::string path; struct stat st{};
    if (!Path(name, path)) return 0;
    if (stat(path.c_str(), &st)) return Fail(errno);
    return S_ISREG(st.st_mode);
}
int32_t OsDirectoryExists(const char* name) {
    std::string path; struct stat st{};
    if (!Path(name, path)) return 0;
    if (stat(path.c_str(), &st)) return Fail(errno);
    return S_ISDIR(st.st_mode);
}
int32_t OsSetFileAttributes(const char* name, uint32_t attributes) {
    std::string path;
    if (!Path(name, path)) return 0;
    // This interface passes Blizzard attributes (directory=0x40, file=0x20),
    // unlike the flags in OS_FILE_DATA. Only readonly has a writable analogue.
    if (attributes & ~0xE1u) return Fail(ENOTSUP);
    struct stat st{};
    if (stat(path.c_str(), &st)) return Fail(errno);
    mode_t mode = attributes & 1 ? st.st_mode & ~0222 : st.st_mode | S_IWUSR;
    return chmod(path.c_str(), mode) == 0 || Fail(errno);
}
int32_t OsDeleteFile(const char* name) {
    std::string path; struct stat st{};
    if (!Path(name, path)) return 0;
    std::lock_guard<std::mutex> lock(handlesMutex);
    if (lstat(path.c_str(), &st)) return Fail(errno);
    if (S_ISDIR(st.st_mode)) return Fail(EISDIR);
    if (!Mutable(st)) return 0;
    if (!S_ISLNK(st.st_mode) && !(st.st_mode & 0222)) return Fail(EACCES);
    return unlink(path.c_str()) == 0 || Fail(errno);
}
int32_t OsMoveFile(const char* from, const char* to) {
    std::string source, dest; struct stat st{};
    if (!Path(from, source) || !Path(to, dest)) return 0;
    std::lock_guard<std::mutex> lock(handlesMutex);
    if (lstat(source.c_str(), &st)) return Fail(errno);
    if (!Mutable(st)) return 0;
    return syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, dest.c_str(), 1 /* RENAME_NOREPLACE */) == 0 || Fail(errno);
}
int32_t OsCopyFile(const char* from, const char* to, int32_t failIfExists) {
    std::string source, dest;
    if (!Path(from, source) || !Path(to, dest)) return 0;
    HOSFILE input = OsCreateFile(from, OS_GENERIC_READ, 1, 3, 0x80, OS_FILE_TYPE_ANY);
    if (input == OS_INVALID_FILE) return 0;
    // The input sharing registration prevents truncating an alias of itself.
    HOSFILE output = OsCreateFile(to, OS_GENERIC_WRITE, 0, failIfExists ? 1 : 2, 0x80, OS_FILE_TYPE_ANY);
    if (output == OS_INVALID_FILE) { int error = errno; auto legacy = SErrGetLastError(); OsCloseFile(input); errno = error; SErrSetLastError(legacy); return 0; }
    bool ok = true; char buffer[65536]; uint32_t count;
    while (ok) {
        if (!OsReadFile(input, buffer, sizeof(buffer), &count)) { ok = false; break; }
        if (!count) break;
        uint32_t offset = 0;
        while (offset < count) {
            uint32_t written;
            if (!OsWriteFile(output, buffer + offset, count - offset, &written)) { ok = false; break; }
            if (!written) { Fail(EIO); ok = false; break; }
            offset += written;
        }
    }
    const int error = errno; const auto legacy = SErrGetLastError();
    bool closedOutput = OsCloseFile(output); bool closedInput = OsCloseFile(input);
    if (!ok) { unlink(dest.c_str()); errno = error; SErrSetLastError(legacy); }
    return ok && closedOutput && closedInput;
}
int32_t OsRemoveDirectory(const char* name) {
    std::string path;
    if (!Path(name, path)) return 0;
    return rmdir(path.c_str()) == 0 || Fail(errno);
}
int32_t OsRemoveDirectoryRecurse(const char* name, uint32_t flags) {
    std::string path;
    if (!Path(name, path)) return 0;
    if (flags & ~1u) return Fail(ENOTSUP);
    // Resolve every parent without following symlinks, and retain its descriptor
    // for the final removal. Do not normalize away '..' or symlink components.
    auto supplied = std::filesystem::path(path);
    std::vector<std::string> components;
    for (const auto& component : supplied.relative_path()) {
        auto text = component.string();
        if (text == ".." || text == ".") return Fail(EINVAL);
        if (!text.empty()) components.push_back(text);
    }
    if (components.empty()) return Fail(EINVAL);
    int parent = open(supplied.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return Fail(errno);
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        int next = openat(parent, components[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = errno; close(parent);
        if (next < 0) return Fail(error);
        parent = next;
    }
    const char* leaf = components.back().c_str();
    int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { int error = errno; close(parent); return Fail(error); }
    struct stat st{}, root{}, cwd{};
    if (fstat(fd, &st) || stat("/", &root) || stat(".", &cwd)) { int error = errno; close(fd); close(parent); return Fail(error); }
    if ((st.st_dev == root.st_dev && st.st_ino == root.st_ino) || (st.st_dev == cwd.st_dev && st.st_ino == cwd.st_ino)) { close(fd); close(parent); return Fail(EINVAL); }
    std::lock_guard<std::mutex> lock(handlesMutex);
    bool ok = RemoveContents(fd, flags & 1);
    int error = errno; auto legacy = SErrGetLastError(); close(fd);
    if (ok && unlinkat(parent, leaf, AT_REMOVEDIR)) { ok = false; error = errno; Fail(error); legacy = SErrGetLastError(); }
    close(parent);
    if (!ok) { errno = error; SErrSetLastError(legacy); }
    return ok;
}
bool OsSetCurrentDirectory(const char* name) {
    std::string path;
    if (!Path(name, path)) return false;
    return chdir(path.c_str()) == 0 || Fail(errno);
}
int32_t OsGetCurrentDirectory(uint32_t size, char* buffer) {
    if (!buffer || !size) return Fail(EINVAL);
    return getcwd(buffer, size) != nullptr || Fail(errno);
}
char* OsFileGetDownloadFolder() {
    // Win64 OsFile.cpp caches the working directory, not the user's Downloads.
    static char folder[260]{};
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!folder[0]) OsGetCurrentDirectory(sizeof(folder), folder);
    return folder;
}
int32_t OsFileList(const char* dir, const char* pattern, OsFileListCallback callback, void* param, int32_t includeHidden) {
    std::string path;
    if (!Path(dir, path) || !pattern || !callback) return Fail(EINVAL);
    // Legacy dir is a search path: discard its last component, including '*'.
    auto separator = path.find_last_of('/');
    if (separator != std::string::npos) path.resize(separator ? separator : 1);
    DIR* directory = opendir(path.c_str());
    if (!directory) return Fail(errno);
    int result = 1;
    for (;;) {
        errno = 0; dirent* entry = readdir(directory);
        if (!entry) { if (errno) result = Fail(errno); break; }
        const char* name = entry->d_name;
        if (!std::strcmp(name, ".") || !std::strcmp(name, "..") || (!includeHidden && name[0] == '.') || !Glob(name, pattern)) continue;
        struct stat st{};
        // The legacy adapter skips entries whose metadata lookup fails.
        if (fstatat(dirfd(directory), name, &st, 0)) continue;
        OS_FILE_DATA data{};
        if (std::strlen(name) >= sizeof(data.fileName)) { result = Fail(ENAMETOOLONG); break; }
        std::strcpy(data.fileName, name);
        data.size = static_cast<uint32_t>(st.st_size);
        data.flags = (S_ISDIR(st.st_mode) ? 0x10 : 0) | (Attributes(st, name) & 3);
        // No handle-registry lock is held while invoking application code.
        if (callback(data, param)) { result = 0; break; }
    }
    int error = errno; closedir(directory); errno = error;
    return result;
}
