#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOPlatform.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/Storm/H/storm.h"

// Isolate the SDK filesystem dispatch from its still-unported shared TLS/error
// stack. This test-only linker wrapper delegates to the real Storm TLS store;
// all filesystem operations below execute real native syscalls.
extern "C" void __wrap__ZN8Blizzard4File12SetLastErrorEi(int32_t error) { SErrSetLastError(error); }

namespace {
using Parms = System_File::Stacked::FileParms;
unsigned failures;
void Check(bool valid, const char* message) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno); }
}
}

int main() {
    char pattern[] = "native-file-test-XXXXXX";
    char* directory = mkdtemp(pattern);
    if (!directory) return 2;
    const auto root = std::filesystem::absolute(directory);
    const auto filename = root / "buffer.bin";
    std::string path = filename.string();
    for (char& character : path) if (character == '/') character = '\\';
    Parms parms{};
    parms.m_path = path.data();
    parms.m_mode = 0x103; // read/write, create always
    Check(System_File::Open(nullptr, &parms) && parms.m_stream, "create file through legacy dispatch flags");
    if (!parms.m_stream) { std::filesystem::remove_all(root); return 2; }
    auto stream = parms.m_stream;
    char original[] = "abcdef";
    parms.m_buffer = original;
    parms.m_amount = 6;
    Check(System_File::Write(nullptr, &parms) && parms.m_amount == 6, "sequential write");
    char patch[] = "XY";
    parms.m_buffer = patch;
    parms.m_amount = 2;
    parms.m_pos = 2;
    Check(System_File::WriteP(nullptr, &parms), "positioned write");
    Check(System_File::GetPos(nullptr, &parms) && parms.m_pos == 6, "positioned write preserves current cursor");
    char buffer[32]{};
    parms.m_buffer = buffer;
    parms.m_amount = sizeof(buffer);
    parms.m_pos = 0;
    Check(System_File::ReadP(nullptr, &parms) && parms.m_amount == 6 && std::memcmp(buffer, "abXYef", 6) == 0,
          "positioned read returns a real short read");
    Check(System_File::GetPos(nullptr, &parms) && parms.m_pos == 6, "positioned read preserves current cursor");
    parms.m_amount = sizeof(buffer);
    Check(System_File::Read(nullptr, &parms) && !parms.m_amount, "read at EOF succeeds with zero bytes");
    parms.m_pos = -1;
    parms.m_amount = 1;
    Check(!System_File::ReadP(nullptr, &parms) && errno == EINVAL && SErrGetLastError() == 8, "negative positioned read is rejected");
    parms.m_pos = (int64_t{1} << 33) + 17;
    char tail = 'Z';
    parms.m_buffer = &tail;
    parms.m_amount = 1;
    Check(System_File::WriteP(nullptr, &parms), "sparse file position exceeds 32 bits");
    parms.m_buffer = buffer;
    Check(System_File::ReadP(nullptr, &parms) && buffer[0] == 'Z', "64-bit positioned read");
    uint32_t boundaryRead = 0;
    Check(System_File::Native::ReadUnbuffered(UINT32_MAX, path.c_str(), buffer, 2, &boundaryRead) &&
          boundaryRead == 2 && !buffer[0] && !buffer[1], "direct read can cross the 4-GiB boundary without wrapping");
    parms.m_path = nullptr;
    Check(System_File::GetFileInfo(nullptr, &parms) && parms.m_info->m_size == (int64_t{1} << 33) + 18,
          "file metadata retains 64-bit size");
    parms.m_pos = 3;
    parms.m_posMode = 0;
    Check(System_File::SetEOF(nullptr, &parms), "truncate file");
    Check(System_File::GetPos(nullptr, &parms) && parms.m_pos == 6, "truncate preserves prior cursor");
    Check(System_File::Flush(nullptr, &parms), "flush file to filesystem");
    Check(System_File::Close(nullptr, &parms) && !parms.m_stream, "close releases file and stream record");
    Check(!System_File::Close(nullptr, &parms) && errno == EBADF, "closed descriptor is rejected");
    parms = {};
    parms.m_path = path.data();
    parms.m_mode = 0x803;
    Check(!System_File::Open(nullptr, &parms) && !parms.m_stream && errno == EEXIST && SErrGetLastError() == 4,
          "exclusive creation does not overwrite existing data");
    parms.m_mode = 0x1001;
    Check(System_File::Open(nullptr, &parms), "open existing file read-only");
    parms.m_buffer = buffer;
    parms.m_amount = 1;
    Check(!System_File::Write(nullptr, &parms) && errno == EBADF, "read-only descriptor cannot write");
    parms.m_mode = 0;
    Check(System_File::SetCacheMode(nullptr, &parms), "cached mode update retains open file");
    Check(System_File::GetPos(nullptr, &parms) && parms.m_pos == 0, "cache update does not move the cursor");
    Check(System_File::Close(nullptr, &parms), "read-only close");
    timespec times[] = {{946684800, 7}, {946684801, 123456789}};
    Check(utimensat(AT_FDCWD, filename.c_str(), times, 0) == 0, "fixture timestamp setup");
    Blizzard::File::FileInfo info{};
    parms.m_info = &info;
    Check(System_File::GetFileInfo(nullptr, &parms) && info.m_size == 3 && info.m_modTime == 1123456789 && info.m_accessTime == 7,
          "native timestamps use nanoseconds since the Blizzard 2000 epoch");
    Check(System_File::Exists(nullptr, &parms) && info.m_existence == 1, "file existence result");
    uint32_t alignment = 0;
    Check(System_File::Native::GetFileAlignment(path.c_str(), alignment) && alignment >= sizeof(void*) && !(alignment & (alignment - 1)),
          "native direct-I/O alignment is a nonzero power of two");
    std::memset(buffer, 0x5A, sizeof(buffer));
    uint32_t received = 99;
    Check(System_File::Native::ReadUnbuffered(1, path.c_str(), buffer + 1, 20, &received) && received == 2 &&
          buffer[0] == 0x5A && buffer[1] == 'b' && buffer[2] == 'X' && buffer[3] == 0x5A,
          "unaligned direct read copies only available bytes and preserves guards");
    Check(System_File::Native::ReadUnbuffered(100, path.c_str(), buffer, sizeof(buffer), &received) && !received,
          "direct read beyond EOF does not underflow prefix subtraction");
    Check(System_File::Native::ReadUnbuffered(0, path.c_str(), nullptr, 0, &received) && !received,
          "zero-length direct read accepts a null buffer");
    Check(!System_File::Native::ReadUnbuffered(0, path.c_str(), nullptr, 1, &received) && errno == EINVAL && !received,
          "nonempty direct read rejects null output");
    parms.m_has.m_all = 4;
    info.m_attributes = 1;
    Check(System_File::SetAttributes(nullptr, &parms) && System_File::IsReadOnly(nullptr, &parms), "read-only attribute maps to native permissions");
    parms.m_has.m_all = 4;
    info.m_attributes = 0;
    Check(System_File::SetAttributes(nullptr, &parms) && !System_File::IsReadOnly(nullptr, &parms), "clearing read-only restores owner write permission");
    parms.m_has.m_all = 4;
    info.m_attributes = 2;
    Check(!System_File::SetAttributes(nullptr, &parms) && errno == ENOTSUP, "unsupported Windows-only attributes do not report fake success");
    std::string missing = (root / "missing").string();
    parms.m_path = missing.data();
    Check(System_File::Exists(nullptr, &parms) && info.m_existence == 0, "missing file existence is a successful negative query");
    parms.m_mode = 0x1001;
    Check(!System_File::Open(nullptr, &parms) && errno == ENOENT && SErrGetLastError() == 4, "open-existing never creates a missing file");
    (void)stream; // The saved identity is never dereferenced after Close.
    std::string nested = (root / "one/two").string();
    parms = {};
    parms.m_path = nested.data();
    parms.m_recursive = true;
    Check(System_File::CreateDirectory(nullptr, &parms) && System_File::CreateDirectory(nullptr, &parms),
          "recursive directory creation accepts an existing directory");
    parms.m_grantEveryone = true;
    Check(!System_File::CreateDirectory(nullptr, &parms) && errno == ENOTSUP,
          "unsupported Windows ACL request is not fabricated as success");
    std::string copied = (root / "copy.bin").string();
    parms = {};
    parms.m_path = path.data();
    parms.m_newpath = copied.data();
    Check(System_File::Copy(nullptr, &parms), "native file copy");
    Check(!System_File::Copy(nullptr, &parms) && errno == EEXIST, "copy no-overwrite contract");
    parms.m_overwrite = true;
    Check(System_File::Copy(nullptr, &parms), "explicit copy overwrite");
    Check(!System_File::Move(nullptr, &parms) && errno == EEXIST && std::filesystem::exists(filename),
          "move never replaces an existing destination");
    std::string moved = (root / "moved.bin").string();
    parms.m_path = copied.data();
    parms.m_newpath = moved.data();
    Check(System_File::Move(nullptr, &parms) && !std::filesystem::exists(copied), "atomic non-replacing move");
    std::vector<std::string> entries;
    std::string rootName = root.string();
    parms = {};
    parms.m_path = rootName.data();
    parms.m_cookie = &entries;
    parms.m_sorted = true;
    parms.m_dircallback = [](const Blizzard::File::ProcessDirParms& record) {
        static_cast<std::vector<std::string>*>(record.m_cookie)->emplace_back(record.m_filename);
        return true;
    };
    Check(System_File::ProcessDirFast(nullptr, &parms) && entries == std::vector<std::string>({"buffer.bin", "moved.bin", "one"}),
          "sorted traversal supplies real entries without dot-directory records");
    parms.m_dircallback = [](const Blizzard::File::ProcessDirParms&) { return false; };
    Check(!System_File::ProcessDirFast(nullptr, &parms), "directory callback can stop traversal");
    Check(System_File::GetFreeSpace(nullptr, &parms) && parms.m_pos > 0, "native available-space query");
    char absolute[4096];
    parms.m_cwdbuf = absolute;
    parms.m_cwdbuflen = sizeof(absolute);
    Check(System_File::MakeAbsolutePath(nullptr, &parms) && root == std::filesystem::path(absolute), "absolute path mapping");
    Check(System_File::GetRootChars(nullptr, &parms) && parms.m_pos == 1 && System_File::IsAbsolutePath(nullptr, &parms),
          "POSIX root and absolute-path contracts");
    parms.m_path = moved.data();
    Check(System_File::Delete(nullptr, &parms) && !std::filesystem::exists(moved), "native unlink");
    parms.m_path = nested.data();
    Check(System_File::RemoveDirectory(nullptr, &parms), "remove empty directory");
    Check(!System_File::Create(nullptr, &parms) && errno == ENOTSUP && SErrGetLastError() == 9,
          "unsupported filesystem-layer creation matches original failure contract");
    std::filesystem::remove_all(root);
    if (!failures) std::puts("PASS: native file dispatch, flags, I/O, EOF, 64-bit offsets, metadata and failures");
    return failures ? 1 : 0;
}
