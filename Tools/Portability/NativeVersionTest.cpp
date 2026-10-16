#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/Source/System/Version/Version.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ChecksumExecutables.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

char* GetComponentVersionData(uint32_t* size);
namespace {
unsigned failures;
unsigned finishes;
int fileError;
int readMode;
uint64_t reportedSize;
std::vector<uint32_t> readRequests;
struct Fatal {};
void Check(bool valid, const char* expression, int line) {
    if (!valid) { ++failures; std::fprintf(stderr, "%d: %s\n", line, expression); }
}
#define CHECK(e) Check(!!(e), #e, __LINE__)
struct Capture : IOsVersionHashCallback {
    unsigned calls = 0;
    std::vector<uint8_t> bytes;
    void Process(const void* data, uint32_t size) override {
        ++calls;
        const auto* first = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
};
}

// Link-time seams inject I/O faults and observe the real consumer's fatal edge;
// no replacement checksum or metadata algorithm is used.
extern "C" uint64_t __real__Z13OsGetFileSizeP9HOSFILE__(HOSFILE file);
extern "C" uint64_t __wrap__Z13OsGetFileSizeP9HOSFILE__(HOSFILE file) {
    return reportedSize ? reportedSize : __real__Z13OsGetFileSizeP9HOSFILE__(file);
}
extern "C" int32_t __real__Z10OsReadFileP9HOSFILE__PvjPj(HOSFILE, void*, uint32_t, uint32_t*);
extern "C" int32_t __wrap__Z10OsReadFileP9HOSFILE__PvjPj(HOSFILE file, void* data, uint32_t size, uint32_t* read) {
    readRequests.push_back(size);
    CHECK(size <= 1024 * 1024);
    if (readMode == 1 || (readMode == 3 && readRequests.size() == 2)) { *read = 0; return 0; }
    if (readMode == 2 || (readMode == 4 && readRequests.size() == 2)) { *read = size ? size - 1 : 0; return 1; }
    return __real__Z10OsReadFileP9HOSFILE__PvjPj(file, data, size, read);
}
extern "C" void __wrap__ZN8Blizzard4File12SetLastErrorEi(int error) { fileError = error; }
extern "C" void __real__ZN4SHA16FinishERNS_6DigestE(SHA1*, SHA1::Digest&);
extern "C" void __wrap__ZN4SHA16FinishERNS_6DigestE(SHA1* self, SHA1::Digest& digest) {
    ++finishes;
    __real__ZN4SHA16FinishERNS_6DigestE(self, digest);
}
extern "C" [[noreturn]] void __wrap__Z26SErrDisplayAppFatalMessagePKcS0_i(const char* message, const char* file, int line) {
    CHECK(!std::strcmp(message, "Native executable authentication checksum is unavailable"));
    CHECK(std::strstr(file, "WoW/Source/WowSvcs/WowSvcsClient/ChecksumExecutables.cpp"));
    CHECK(line == 26);
    throw Fatal{};
}

int main() {
    char name[] = "/tmp/native-version-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return 2;
    close(fd);
    // Synthetic ELF, PE-like, truncated and overflowing resource-like bytes.
    // Metadata is unavailable regardless of contents: these are NOT parser tests.
    const std::vector<std::vector<uint8_t>> fixtures = {
        {}, {0x7f, 'E', 'L', 'F', 2, 1, 1}, {'M', 'Z'},
        {'M', 'Z', 0xff, 0xff, 0xff, 0xff},
        {0xbd, 0x04, 0xef, 0xfe, 0xff, 0xff, 0xff, 0xff},
        std::vector<uint8_t>(1024 * 1024 + 8193, 0x5a),
        std::vector<uint8_t>(8193, 0xa5)
    };
    for (const auto& fixture : fixtures) {
        FILE* file = std::fopen(name, "wb");
        if (!file) return 2;
        CHECK(fixture.empty() || std::fwrite(fixture.data(), 1, fixture.size(), file) == fixture.size());
        CHECK(!std::fclose(file));
        Capture capture;
        readRequests.clear();
        CHECK(IOsVersionChecksum(capture, name, 0) == 1);
        CHECK(capture.calls == 1 && capture.bytes == fixture);
        CHECK(readRequests.size() == (fixture.size() > 1024 * 1024 ? 2 : 1));
        if (fixture.size() > 1024 * 1024) {
            CHECK(readRequests[0] == 1024 * 1024 && readRequests[1] == 8193);
            for (int mode : {3, 4}) {
                readMode = mode;
                readRequests.clear();
                Capture failed;
                CHECK(!IOsVersionChecksum(failed, name, 0) && !failed.calls);
                CHECK(readRequests.size() == 2);
            }
            readMode = 0;
        }
        Capture loaded;
        CHECK(!IOsVersionChecksum(loaded, name, 1));
        CHECK(errno == ENOTSUP && SErrGetLastError() == 50 && !loaded.calls);
        Blizzard::Version::FileVersion version{1, 2, 3, 4};
        CHECK(!Blizzard::Version::GetFileVersion(name, version));
        CHECK(!version.m_major && !version.m_minor && !version.m_build && !version.m_revision);
        CHECK(errno == ENOTSUP && fileError == 50);
        OsFileVersionInfo info(name);
        CHECK(!info.m_data.Count());
        for (auto value : info.m_fileVersion) CHECK(!value);
        for (auto value : info.m_productVersion) CHECK(!value);
    }
    for (int mode : {1, 2}) {
        readMode = mode;
        Capture capture;
        CHECK(!IOsVersionChecksum(capture, name, 0) && !capture.calls);
    }
    readMode = 0;
    // Report large lengths for the real small fixture. The first real bounded
    // read must be short and fail without callback or a huge allocation.
    for (uint64_t boundary : {uint64_t{0x80000000}, uint64_t{0xffffffff}}) {
        reportedSize = boundary;
        readRequests.clear();
        Capture failed;
        CHECK(!IOsVersionChecksum(failed, name, 0) && !failed.calls);
        CHECK(errno == EIO && SErrGetLastError() == 30);
        CHECK(readRequests.size() == 1 && readRequests[0] == 1024 * 1024);
    }
    reportedSize = uint64_t{1} << 32;
    readRequests.clear();
    Capture capture;
    CHECK(!IOsVersionChecksum(capture, name, 0) && !capture.calls && errno == EFBIG);
    CHECK(readRequests.empty());
    reportedSize = 0;
    CHECK(!IOsVersionChecksum(capture, nullptr, 0) && !capture.calls && errno == EINVAL);
    CHECK(!IOsVersionChecksum(capture, nullptr, 1) && !capture.calls && errno == ENOTSUP);
    CHECK(!IOsVersionChecksum(capture, name, -1) && !capture.calls && errno == ENOTSUP);
    CHECK(!unlink(name));
    CHECK(!IOsVersionChecksum(capture, name, 0) && !capture.calls);
    CHECK(!IOsVersionChecksum(capture, "/tmp", 0) && !capture.calls);
    for (const char* path : {static_cast<const char*>(nullptr), ""}) {
        Blizzard::Version::FileVersion version{1, 2, 3, 4};
        CHECK(!Blizzard::Version::GetFileVersion(path, version));
        CHECK(!version.m_major && !version.m_minor && !version.m_build && !version.m_revision);
        OsFileVersionInfo info(path);
        CHECK(!info.m_data.Count());
    }
    uint32_t size = 99;
    CHECK(!GetComponentVersionData(&size) && !size && errno == ENOTSUP && fileError == 50);
    CHECK(!GetComponentVersionData(nullptr) && errno == EINVAL && fileError == 87);

    std::array<uint8_t, 20> output;
    output.fill(0xa5);
    const uint8_t challenge[] = {1, 2, 3};
    bool fatal = false;
    try { ChecksumExecutables(challenge, sizeof(challenge), output.data()); }
    catch (const Fatal&) { fatal = true; }
    CHECK(fatal && !finishes);
    CHECK(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0xa5; }));
    std::printf("NativeVersion: %u failures\n", failures);
    return failures ? 1 : 0;
}
