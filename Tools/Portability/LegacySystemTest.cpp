#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <string>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include "Engine/Source/Os/Path.h"
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #expression, errno); std::exit(1); } } while (0)

namespace {
int metadataMode = 0;
int pathMode = 0;
int userRetries = 0;
const char* fixturePath = "/tmp/generated-\xc3\xa9/client";
}

extern "C" long __real_sysconf(int);
extern "C" long __wrap_sysconf(int name) {
    // Sanitizer runtimes also query page size and other unrelated limits.
    if (name != _SC_NPROCESSORS_ONLN) return __real_sysconf(name);
    return metadataMode ? -1 : 48;
}
extern "C" int __wrap_sysinfo(struct sysinfo* info) noexcept {
    if (metadataMode) { errno = EIO; return -1; }
    *info = {};
    info->totalram = 0x80000000UL;
    info->mem_unit = 4096;
    return 0;
}
extern "C" int __wrap_gethostname(char* name, size_t size) noexcept {
    if (metadataMode) { errno = EIO; return -1; }
    CHECK(size > std::strlen("fixture-host"));
    std::strcpy(name, "fixture-host");
    return 0;
}
extern "C" int __wrap_uname(struct utsname* info) noexcept {
    if (metadataMode) { errno = EIO; return -1; }
    *info = {};
    std::strcpy(info->sysname, "Linux");
    std::strcpy(info->release, "fixture-release");
    std::strcpy(info->machine, "fixture-arch");
    return 0;
}
extern "C" int __wrap_getpwuid_r(uid_t, struct passwd* entry, char* storage, size_t size, struct passwd** result) {
    *result = nullptr;
    if (metadataMode == 1) return EIO;
    if (metadataMode == 2) return 0;
    if (size < 2048) { ++userRetries; return ERANGE; }
    std::strcpy(storage, "fixture-user");
    entry->pw_name = storage;
    *result = entry;
    return 0;
}
extern "C" ssize_t __real_readlink(const char*, char*, size_t);
extern "C" ssize_t __wrap_readlink(const char* name, char* buffer, size_t size) noexcept {
    CHECK(std::strcmp(name, "/proc/self/exe") == 0);
    if (pathMode == 0) return __real_readlink(name, buffer, size);
    if (pathMode == 2) { errno = ENOENT; return -1; }
    size_t count = std::strlen(fixturePath);
    if (count > size) count = size;
    std::memcpy(buffer, fixturePath, count);
    return static_cast<ssize_t>(count);
}

int main() {
    CHECK(OsGetNumberOfProcessors() == 48);
    CHECK(OsGetPhysicalMemory() == UINT64_C(0x80000000000));
    CHECK(OsGetVersion() == OsType_Linux);
    CHECK(OsGetOsId() == 0x57696E);
#if defined(__i386__) || defined(__x86_64__)
    CHECK(OsGetProcessorId() == 0x783836);
    uint64_t expectedFrequency = 0;
    uint32_t a, b, c, d;
    if (__get_cpuid_max(0, nullptr) >= 0x16) {
        __cpuid(0x16, a, b, c, d);
        expectedFrequency = static_cast<uint64_t>(a & 0xFFFF) * 1000000;
    }
    CHECK(OsGetProcessorTicksPerSecond() == expectedFrequency);
#else
    CHECK(OsGetProcessorId() == 0);
    CHECK(OsGetProcessorTicksPerSecond() == 0);
#endif
    CpuIdInfo cpu{};
    CHECK(OsGetProcessorTicksPerSecond() == OsGetProcessorTicksPerSecond());
    IOsSystemCpuLog(&cpu); // Disabled: no output and no array over-read.
    std::memset(&cpu, 'X', sizeof(cpu));
    OsSystemEnableCpuLog();
    IOsSystemCpuLog(&cpu); // Fixed-width, deliberately nonterminated strings.
    IOsSystemCpuLog(&cpu); // One log per enable.

    char buffer[1024];
    OsGetVersionString(buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "Linux fixture-release fixture-arch") == 0);
    char tiny[] = {'x', 'z'};
    OsGetVersionString(tiny, 1);
    CHECK(tiny[0] == 0 && tiny[1] == 'z');
    OsGetVersionString(tiny, 0);
    OsGetVersionString(nullptr, 10);

    uint32_t size = 0;
    CHECK(!OsGetComputerName(nullptr, &size) && size == 13 && errno == ERANGE);
    size = 12;
    CHECK(!OsGetComputerName(buffer, &size) && size == 13 && buffer[0] == 0);
    CHECK(OsGetComputerName(buffer, &size) && size == 12 && std::strcmp(buffer, "fixture-host") == 0);
    CHECK(!OsGetComputerName(buffer, nullptr) && errno == EINVAL);
    size = 0;
    CHECK(!OsGetUserName(nullptr, &size) && size == 13 && errno == ERANGE);
    size = 12;
    CHECK(!OsGetUserName(buffer, &size) && size == 13 && buffer[0] == 0);
    CHECK(OsGetUserName(buffer, &size) && size == 13 && std::strcmp(buffer, "fixture-user") == 0);
    CHECK(userRetries > 0);
    CHECK(!OsGetUserName(buffer, nullptr) && errno == EINVAL);

    metadataMode = 1;
    CHECK(OsGetNumberOfProcessors() == 1);
    CHECK(OsGetPhysicalMemory() == 0);
    size = sizeof(buffer);
    buffer[0] = 'x';
    CHECK(!OsGetComputerName(buffer, &size) && errno == EIO && !buffer[0]);
    buffer[0] = 'x';
    CHECK(!OsGetUserName(buffer, &size) && errno == EIO && !buffer[0]);
    OsGetVersionString(buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "Linux") == 0);
    metadataMode = 2;
    CHECK(!OsGetUserName(buffer, &size) && errno == ENOENT);
    metadataMode = 0;

    CHECK(OsSleepInBackground() == 1 && OsGetBackgroundSleepMs() == 0);
    OsSetSleepInBackground(-7);
    OsSetBackgroundSleepMs(UINT32_MAX);
    CHECK(OsSleepInBackground() == -7 && OsGetBackgroundSleepMs() == UINT32_MAX);
    OsSetSleepInBackground(0);
    OsSetBackgroundSleepMs(250);
    CHECK(!OsSleepInBackground() && OsGetBackgroundSleepMs() == 250);
    CHECK(unsetenv("SSH_CONNECTION") == 0 && unsetenv("SSH_TTY") == 0);
    CHECK(setenv("DISPLAY", "remote.invalid:0", 1) == 0);
    CHECK(!OsIsRemoteSession());
    CHECK(setenv("SSH_CONNECTION", "fixture", 1) == 0 && OsIsRemoteSession());
    CHECK(setenv("SSH_CONNECTION", "", 1) == 0 && !OsIsRemoteSession());
    CHECK(setenv("SSH_TTY", "/fixture/tty", 1) == 0 && OsIsRemoteSession());

    CHECK(GetExecutablePathUTF8(buffer, sizeof(buffer)) > 0);
    CHECK(buffer[0] == '/');
    pathMode = 1;
    CHECK(GetExecutablePathUTF8(buffer, sizeof(buffer)) == std::strlen(fixturePath));
    CHECK(std::strcmp(buffer, fixturePath) == 0);
    OsGetExePath(buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "/tmp/generated-\xc3\xa9/") == 0);
    CHECK(!GetExecutablePathUTF8(buffer, std::strlen(fixturePath)) && buffer[0] == 0 && errno == ENAMETOOLONG);
    CHECK(GetExecutablePathUTF8(buffer, std::strlen(fixturePath) + 1) == std::strlen(fixturePath));
    CHECK(!GetExecutablePathUTF8(tiny, 1) && tiny[0] == 0 && tiny[1] == 'z');
    CHECK(!GetExecutablePathUTF8(nullptr, 10) && errno == EINVAL);
    CHECK(!GetExecutablePathUTF8(buffer, 0) && errno == EINVAL);
    CHECK(!GetModuleFileNameUTF8(&size, buffer, sizeof(buffer)) && !buffer[0] && errno == ENOTSUP);
    pathMode = 2;
    OsGetExePath(buffer, sizeof(buffer));
    CHECK(!buffer[0] && errno == ENOENT);

    char extensionPath[] = "dir.ext/sub\\file.tar.gz";
    CHECK(std::strcmp(OsPathFindExtensionWithoutDot(extensionPath), "gz") == 0);
    char noExtension[] = "dir.ext/file";
    CHECK(OsPathFindExtensionWithoutDot(noExtension) == noExtension + std::strlen(noExtension));
    char trailingDot[] = "file.";
    CHECK(*OsPathFindExtensionWithoutDot(trailingDot) == 0);
    OsBuildFontFilePath("FRIZQT__.TTF", buffer, sizeof(buffer));
    CHECK(std::strcmp(buffer, "Fonts\\FRIZQT__.TTF") == 0); // Archive path, no font/device lookup.
    std::puts("LegacySystemTest passed");
}
