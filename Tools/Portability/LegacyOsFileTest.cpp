#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
unsigned failures;
bool failNextFdopendir;
int failedDirectoryDescriptor = -1;
bool failedDirectoryDescriptorWasOpen;
void Check(bool valid, const char* expression, int line) {
    if (!valid) { ++failures; std::fprintf(stderr, "line %d: %s (errno=%d legacy=%u)\n", line, expression, errno, SErrGetLastError()); }
}
#define CHECK(expression) Check(!!(expression), #expression, __LINE__)
int DescriptorCount() {
    DIR* directory = opendir("/proc/self/fd");
    CHECK(directory != nullptr);
    if (!directory) return -1;
    int count = 0;
    for (;;) {
        errno = 0;
        auto* entry = readdir(directory);
        if (!entry) { CHECK(errno == 0); break; }
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    }
    CHECK(closedir(directory) == 0);
    return count;
}
HOSFILE Open(const char* name, uint32_t access = OS_GENERIC_READ, uint32_t share = 0, uint32_t disposition = 3) {
    return OsCreateFile(name, access, share, disposition, 0x80, OS_FILE_TYPE_ANY);
}
struct Listing { std::vector<OS_FILE_DATA> entries; bool stop = false; };
int32_t Collect(OS_FILE_DATA& data, void* cookie) {
    auto& list = *static_cast<Listing*>(cookie);
    list.entries.push_back(data);
    // Exercise reentry: enumeration must not hold the handle registry lock.
    auto file = Open("data");
    CHECK(file != OS_INVALID_FILE);
    if (file != OS_INVALID_FILE) CHECK(OsCloseFile(file));
    return list.stop;
}
}
// Failure-only injection: every unarmed call delegates to the real libc provider.
extern "C" DIR* __real_fdopendir(int fd);
extern "C" DIR* __wrap_fdopendir(int fd) {
    if (failNextFdopendir) {
        failNextFdopendir = false;
        failedDirectoryDescriptor = fd;
        failedDirectoryDescriptorWasOpen = fcntl(fd, F_GETFD) >= 0;
        errno = ENOMEM;
        return nullptr;
    }
    return __real_fdopendir(fd);
}
int main() {
    namespace fs = std::filesystem;
    char original[4096];
    if (!getcwd(original, sizeof(original))) return 2;
    char fixture[] = "/tmp/legacy-os-file-XXXXXX";
    char* temporary = mkdtemp(fixture);
    if (!temporary) return 2;
    const std::string root = temporary;
    CHECK(OsSetCurrentDirectory(root.c_str()));
    CHECK(OsCreateDirectory("nested\\child", 1));
    CHECK(!OsCreateDirectory("nested", 0));
    CHECK(OsDirectoryExists("nested\\child"));
    CHECK(!OsFileExists("nested"));
    char cwd[4096];
    CHECK(OsGetCurrentDirectory(sizeof(cwd), cwd) == 1 && root == cwd);
    CHECK(!OsGetCurrentDirectory(1, cwd));
    CHECK(!OsGetCurrentDirectory(0, nullptr));
    CHECK(std::string(OsFileGetDownloadFolder()) == root);
    CHECK(OsSetCurrentDirectory("nested"));
    CHECK(std::string(OsFileGetDownloadFolder()) == root);
    CHECK(OsSetCurrentDirectory(root.c_str()));
    CHECK(!OsSetCurrentDirectory(nullptr));
    CHECK(!OsSetCurrentDirectory("absent"));

    HOSFILE file = Open("data", OS_GENERIC_READ | OS_GENERIC_WRITE, 0, 1);
    CHECK(file != OS_INVALID_FILE);
    uint32_t count = 99;
    CHECK(OsWriteFile(file, "abcdef", 6, &count) && count == 6);
    CHECK(OsGetFileSize(file) == 6);
    CHECK(!OsReadFile(file, nullptr, 1, &count) && !count && errno == EINVAL);
    char buffer[32]{};
    CHECK(!OsReadFile(file, buffer, UINT32_MAX, &count) && !count && errno == EINVAL);
    CHECK(!OsWriteFile(file, buffer, 1, nullptr));
    CHECK(OsCloseFile(file));
    CHECK(!OsCloseFile(file) && errno == EBADF && SErrGetLastError() == 6);
    CHECK(OsGetFileSize(file) == UINT32_MAX && errno == EBADF);
    HOSFILE replacement = Open("data");
    CHECK(replacement != OS_INVALID_FILE && replacement != file);
    CHECK(!OsCloseFile(file));
    CHECK(OsReadFile(replacement, buffer, sizeof(buffer), &count) && count == 6 && !std::memcmp(buffer, "abcdef", 6));
    CHECK(OsReadFile(replacement, buffer, sizeof(buffer), &count) && count == 0);
    CHECK(!OsWriteFile(replacement, "x", 1, &count) && count == 0 && errno == EBADF);
    CHECK(OsCloseFile(replacement));
    CHECK(!OsCloseFile(nullptr));
    CHECK(!OsReadFile(OS_INVALID_FILE, buffer, 1, &count) && !count);

    int savedStdin = dup(0);
    close(0);
    file = Open("data");
    CHECK(file != OS_INVALID_FILE);
    CHECK(OsReadFile(file, buffer, 1, &count) && count == 1 && buffer[0] == 'a');
    CHECK(OsCloseFile(file));
    CHECK(fcntl(0, F_GETFD) == -1 && errno == EBADF);
    if (savedStdin >= 0) { CHECK(dup2(savedStdin, 0) == 0); close(savedStdin); }

    CHECK(Open("missing") == OS_INVALID_FILE && errno == ENOENT && SErrGetLastError() == 2);
    CHECK(Open(nullptr) == OS_INVALID_FILE && errno == EINVAL);
    CHECK(Open("") == OS_INVALID_FILE);
    CHECK(Open("nested") == OS_INVALID_FILE && errno == ENOTSUP);
    CHECK(Open("data", 0) == OS_INVALID_FILE);
    CHECK(Open("data", 1) == OS_INVALID_FILE);
    CHECK(Open("data", OS_GENERIC_READ, 0, 0) == OS_INVALID_FILE);
    CHECK(Open("data", OS_GENERIC_READ, 4) == OS_INVALID_FILE && errno == ENOTSUP);
    CHECK(OsCreateFile("data", OS_GENERIC_READ, 0, 3, 0x100, OS_FILE_TYPE_ANY) == OS_INVALID_FILE && errno == ENOTSUP);
    CHECK(OsCreateFile("data", OS_GENERIC_READ, 0, 3, 0x80, 42) == OS_INVALID_FILE && errno == ENOTSUP);
    CHECK(Open("data", OS_GENERIC_WRITE, 0, 1) == OS_INVALID_FILE && errno == EEXIST);
    CHECK(Open("missing", OS_GENERIC_WRITE, 0, 5) == OS_INVALID_FILE && errno == ENOENT);
    CHECK(Open("data", OS_GENERIC_READ, 0, 5) == OS_INVALID_FILE && errno == EINVAL);
    file = Open("created", OS_GENERIC_WRITE, 0, 4);
    CHECK(file != OS_INVALID_FILE && OsCloseFile(file));
    file = Open("data", OS_GENERIC_WRITE, 0, 4);
    CHECK(file != OS_INVALID_FILE && OsGetFileSize(file) == 6 && OsCloseFile(file));
    file = Open("created", OS_GENERIC_WRITE, 0, 5);
    CHECK(file != OS_INVALID_FILE && OsGetFileSize(file) == 0 && OsCloseFile(file));

    CHECK(link("data", "alias") == 0);
    file = Open("data", OS_GENERIC_READ, 1);
    replacement = Open("alias", OS_GENERIC_READ, 1);
    CHECK(file != OS_INVALID_FILE && replacement != OS_INVALID_FILE);
    CHECK(Open("alias", OS_GENERIC_WRITE, 3, 2) == OS_INVALID_FILE && SErrGetLastError() == 32);
    CHECK(OsGetFileSize(file) == 6); // denied create-always must not truncate
    CHECK(Open("data", OS_GENERIC_READ, 0) == OS_INVALID_FILE);
    CHECK(!OsDeleteFile("data") && SErrGetLastError() == 32);
    CHECK(!OsMoveFile("alias", "moved") && SErrGetLastError() == 32);
    CHECK(OsCloseFile(replacement) && OsCloseFile(file));
    file = Open("data", OS_GENERIC_READ | OS_GENERIC_WRITE, 3);
    replacement = Open("alias", OS_GENERIC_WRITE, 3);
    CHECK(file != OS_INVALID_FILE && replacement != OS_INVALID_FILE);
    CHECK(OsCloseFile(replacement) && OsCloseFile(file));

    CHECK(OsGetFileAttributes(nullptr) == 0 && errno == EINVAL);
    CHECK(OsGetFileAttributes("missing") == UINT32_MAX);
    CHECK(OsGetFileAttributes("nested") == 0x40);
    CHECK(OsGetFileAttributes("data") == 0x20);
    CHECK(OsSetFileAttributes("data", 0x21));
    CHECK(OsGetFileAttributes("data") == 0x21);
    CHECK(!OsDeleteFile("data") && errno == EACCES);
    CHECK(!OsSetFileAttributes("data", 2) && errno == ENOTSUP);
    CHECK(OsSetFileAttributes("data", 0x20));
    CHECK(OsGetFileAttributes("data") == 0x20);
    CHECK(OsCopyFile("data", "copy", 1));
    CHECK(!OsCopyFile("data", "copy", 1) && errno == EEXIST);
    CHECK(OsCopyFile("data", "copy", 0));
    CHECK(!OsCopyFile("data", "alias", 0) && SErrGetLastError() == 32);
    CHECK(!OsCopyFile("data", "data", 0));
    file = Open("copy");
    CHECK(file != OS_INVALID_FILE && OsGetFileSize(file) == 6);
    CHECK(OsReadFile(file, buffer, sizeof(buffer), &count) && count == 6 && !std::memcmp(buffer, "abcdef", 6));
    CHECK(OsCloseFile(file));
    CHECK(!OsMoveFile("copy", "data") && errno == EEXIST);
    CHECK(OsMoveFile("copy", "moved"));
    CHECK(!OsFileExists("copy") && OsFileExists("moved"));
    CHECK(OsDeleteFile("moved"));
    CHECK(!OsDeleteFile("nested"));
    CHECK(!OsCopyFile(nullptr, "x", 0));
    CHECK(!OsMoveFile("absent", "x"));
    CHECK(!OsDeleteFile("absent"));

    int sparse = open("sparse", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    CHECK(sparse >= 0);
    constexpr off_t size = (off_t{1} << 33) + 17;
    CHECK(ftruncate(sparse, size) == 0);
    CHECK(pwrite(sparse, "Z", 1, size - 1) == 1);
    close(sparse);
    file = Open("sparse");
    CHECK(file != OS_INVALID_FILE && OsGetFileSize(file) == static_cast<uint64_t>(size));
    CHECK(OsCloseFile(file));
    // Only metadata/enumeration is exercised on the sparse >4GiB fixture.
    Listing listing;
    CHECK(OsFileList("./*", "sparse", Collect, &listing, 0));
    CHECK(listing.entries.size() == 1 && listing.entries[0].size == 17);
    file = Open(".hidden", OS_GENERIC_WRITE, 0, 1);
    CHECK(file != OS_INVALID_FILE && OsCloseFile(file));
    CHECK(OsGetFileAttributes(".hidden") & 2);
    listing.entries.clear();
    CHECK(OsFileList("./*", ".*", Collect, &listing, 0) && listing.entries.empty());
    CHECK(OsFileList("./*", ".*", Collect, &listing, 1) && listing.entries.size() == 1 && listing.entries[0].flags == 2);
    listing.entries.clear();
    CHECK(OsFileList("./*", "D?T*", Collect, &listing, 0) && listing.entries.size() == 1);
    listing.entries.clear();
    CHECK(OsFileList("./*", "nested", Collect, &listing, 0) && listing.entries.size() == 1 && listing.entries[0].flags == 0x10);
    CHECK(symlink("missing-target", "broken") == 0);
    listing.entries.clear();
    CHECK(OsFileList("./*", "broken", Collect, &listing, 1) && listing.entries.empty());
    listing.entries.clear(); listing.stop = true;
    SErrSetLastError(12345);
    CHECK(!OsFileList("./*", "*", Collect, &listing, 1) && listing.entries.size() == 1);
    CHECK(SErrGetLastError() == 12345);
    CHECK(!OsFileList("absent/*", "*", Collect, &listing, 1));
    CHECK(!OsFileList("./*", nullptr, Collect, &listing, 1));
    CHECK(!OsFileList("./*", "*", nullptr, &listing, 1));

    // Real short write, followed by a real error, without filling a filesystem.
    rlimit limit{};
    CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0);
    auto oldSignal = std::signal(SIGXFSZ, SIG_IGN);
    rlimit small = limit; small.rlim_cur = 3;
    CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    file = Open("short", OS_GENERIC_WRITE, 0, 1);
    CHECK(file != OS_INVALID_FILE);
    CHECK(OsWriteFile(file, "abcdef", 6, &count) && count == 3);
    CHECK(!OsWriteFile(file, "abcdef", 6, &count) && !count && errno == EFBIG);
    CHECK(OsCloseFile(file));
    CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
    std::signal(SIGXFSZ, oldSignal);

    CHECK(symlink("data", "link") == 0);
    CHECK(OsFileExists("link"));
    CHECK(OsDeleteFile("link") && OsFileExists("data"));
    CHECK(OsCreateDirectory("outside", 0));
    CHECK(OsCopyFile("data", "outside/keep", 1));
    CHECK(OsCreateDirectory("tree/sub", 1));
    CHECK(OsCopyFile("data", "tree/sub/readonly", 1));
    const int descriptorsBeforeFailure = DescriptorCount();
    CHECK(descriptorsBeforeFailure >= 0);
    failNextFdopendir = true;
    CHECK(!OsRemoveDirectoryRecurse("tree", 1));
    CHECK(errno == ENOMEM && SErrGetLastError() == 8);
    CHECK(!failNextFdopendir && failedDirectoryDescriptorWasOpen);
    CHECK(failedDirectoryDescriptor >= 0);
    CHECK(fcntl(failedDirectoryDescriptor, F_GETFD) == -1 && errno == EBADF);
    CHECK(DescriptorCount() == descriptorsBeforeFailure);
    CHECK(OsFileExists("tree/sub/readonly"));
    CHECK(OsSetFileAttributes("tree/sub/readonly", 1));
    CHECK(symlink("../../outside", "tree/sub/link") == 0);
    CHECK(!OsRemoveDirectoryRecurse("tree", 0) && errno == EACCES);
    CHECK(OsRemoveDirectoryRecurse("tree", 1));
    CHECK(OsFileExists("outside/keep"));
    CHECK(symlink("outside", "dirlink") == 0);
    CHECK(!OsRemoveDirectoryRecurse("dirlink", 1));
    CHECK(!OsRemoveDirectoryRecurse("dirlink/keep", 1));
    CHECK(OsFileExists("outside/keep"));
    CHECK(!OsRemoveDirectoryRecurse("/", 1));
    CHECK(!OsRemoveDirectoryRecurse(".", 1));
    CHECK(!OsRemoveDirectoryRecurse("nested/..", 1));
    CHECK(!OsRemoveDirectoryRecurse(root.c_str(), 1));
    CHECK(!OsRemoveDirectoryRecurse("outside", 2) && errno == ENOTSUP);
    CHECK(!OsRemoveDirectory("outside"));
    CHECK(OsRemoveDirectoryRecurse("outside", 0));
    CHECK(OsRemoveDirectory("nested/child"));
    CHECK(OsRemoveDirectory("nested"));
    CHECK(OsSetCurrentDirectory(original));
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    CHECK(!cleanupError);
    std::fprintf(stdout, "LegacyOsFileTest: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
