#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Os/Posix/Path.h"
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct FD {
    int fd = -1;
    ~FD() { if (fd >= 0) close(fd); }
};
bool Fail(int error) { SErrSetLastError(OsPosixFileError(error)); return false; }
std::string Encode(const std::string& text) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        result += hex[c >> 4]; result += hex[c & 15];
    }
    return result;
}
bool Location(const char* key, const char* name, uint32_t flags, FD& directory, std::string& entry) {
    if (!key || !*key || !name || !*name || (flags & ~31u)) return Fail(EINVAL);
    // There is deliberately no machine-wide fallback or machine-wide write.
    if (flags & SREG_EXCLUDE_CURRENT_USER) return Fail(EACCES);
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path root;
    if (xdg && *xdg == '/') root = xdg;
    else if (home && *home == '/') root = std::filesystem::path(home) / ".config";
    else return Fail(ENOENT);
    root /= "wowdecomp/storm";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return Fail(error.value());
    directory.fd = open(root.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (directory.fd < 0) return Fail(errno);
    struct stat st{};
    if (fstat(directory.fd, &st)) return Fail(errno);
    if (st.st_uid != geteuid()) return Fail(EACCES);
    if (fchmod(directory.fd, 0700)) return Fail(errno);
    std::string full = flags & SREG_ABSOLUTE ? "" :
        (flags & SREG_BATTLE_NET ? "Software\\Battle.net\\" : "Software\\Blizzard Entertainment\\");
    full += key;
    entry = Encode(full) + "-" + Encode(name);
    return true;
}
bool Transfer(const char* key, const char* name, uint32_t flags, std::string& data, bool save) {
    FD dir, lock;
    std::string entry;
    if (!Location(key, name, flags, dir, entry)) return false;
    lock.fd = openat(dir.fd, ".lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock.fd < 0 || flock(lock.fd, save ? LOCK_EX : LOCK_SH)) return Fail(errno);
    FD file;
    if (!save) {
        file.fd = openat(dir.fd, entry.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (file.fd < 0) return Fail(errno);
        struct stat st{};
        if (fstat(file.fd, &st)) return Fail(errno);
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_size < 2 || st.st_size > 1048576) return Fail(EINVAL);
        data.clear();
        char buffer[4096];
        for (;;) {
            ssize_t count = read(file.fd, buffer, sizeof(buffer));
            if (count < 0) { if (errno == EINTR) continue; return Fail(errno); }
            if (!count) break;
            data.append(buffer, count);
            if (data.size() > 1048576) return Fail(EINVAL);
        }
        if (data.size() < 2 || data[1] != '\n' || (data[0] != 's' && data[0] != 'u') || data.find('\0') != std::string::npos) return Fail(EINVAL);
        return true;
    }
    if (data.size() > 1048576) return Fail(EINVAL);
    // The directory lock also serializes the fixed temporary name across processes.
    file.fd = openat(dir.fd, ".pending", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file.fd < 0 && errno == EEXIST) {
        if (unlinkat(dir.fd, ".pending", 0)) return Fail(errno);
        file.fd = openat(dir.fd, ".pending", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    if (file.fd < 0) return Fail(errno);
    int error = 0;
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t count = write(file.fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += count;
    }
    if (!error && fsync(file.fd)) error = errno;
    if (!error && renameat(dir.fd, ".pending", dir.fd, entry.c_str())) error = errno;
    if (!error && fsync(dir.fd)) error = errno;
    if (error) { unlinkat(dir.fd, ".pending", 0); return Fail(error); }
    return true;
}
bool Number(const std::string& data, uint32_t& value) {
    if (data[0] == 'u') {
        auto result = std::from_chars(data.data() + 2, data.data() + data.size(), value);
        return result.ec == std::errc{} && result.ptr == data.data() + data.size();
    }
    char* end;
    errno = 0;
    unsigned long parsed = std::strtoul(data.c_str() + 2, &end, 0);
    if (errno || end == data.c_str() + 2 || *end || parsed > UINT32_MAX) return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}
}
int32_t STORMAPI SRegSaveString(const char* key, const char* name, uint32_t flags, const char* value) {
    if (!value) return Fail(EINVAL);
    std::string data = std::string("s\n") + value;
    return Transfer(key, name, flags, data, true);
}
int32_t STORMAPI SRegSaveValue(const char* key, const char* name, uint32_t flags, uint32_t value) {
    std::string data = "u\n" + std::to_string(value);
    return Transfer(key, name, flags, data, true);
}
int32_t STORMAPI SRegLoadString(const char* key, const char* name, uint32_t flags, char* buffer, uint32_t size) {
    if (!buffer || !size) return Fail(EINVAL);
    std::string data;
    if (!Transfer(key, name, flags, data, false)) return 0;
    if (data[0] == 'u') { uint32_t value; if (!Number(data, value)) return Fail(EINVAL); data = "s\n" + std::to_string(value); }
    if (data.size() - 2 >= size) { SErrSetLastError(234); return 0; }
    std::memcpy(buffer, data.c_str() + 2, data.size() - 1);
    return 1;
}
int32_t STORMAPI SRegLoadValue(const char* key, const char* name, uint32_t flags, uint32_t* value) {
    if (!value) return Fail(EINVAL);
    std::string data;
    if (!Transfer(key, name, flags, data, false)) return 0;
    uint32_t parsed;
    if (!Number(data, parsed)) return Fail(EINVAL);
    *value = parsed;
    return 1;
}
