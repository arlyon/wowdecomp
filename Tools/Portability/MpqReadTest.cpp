#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/MPQEncrypt.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/SFile.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Mopaq/SFileArchives.h"
#include <array>
#include <atomic>
#include <thread>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

namespace {
unsigned failures;
void Check(bool ok, const char* expression, int line) {
    if (!ok) { ++failures; std::fprintf(stderr, "%d: %s\n", line, expression); }
}
#define CHECK(e) Check(!!(e), #e, __LINE__)
}

int main() {
    static_assert(std::is_same<decltype(System_SFile::s_lastError), std::atomic<uint32_t>>::value,
        "Asynchronous archive errors must not race on plain global storage");
    const auto previousError = Blizzard::Mopaq::SFileGetLastError();
    std::thread otherOwner([] { System_SFile::s_lastError = 0x12345678; });
    otherOwner.join();
    CHECK(Blizzard::Mopaq::SFileGetLastError() == 0x12345678); // Still process-global, not thread-local.
    System_SFile::s_lastError = 0x60000000;
    std::atomic<bool> invalidError{false};
    std::array<std::thread, 4> writers;
    for (unsigned i = 0; i < writers.size(); ++i) {
        writers[i] = std::thread([&, i] {
            for (unsigned attempt = 0; attempt < 5000; ++attempt) {
                System_SFile::s_lastError = 0x60000000 + i;
                const auto error = Blizzard::Mopaq::SFileGetLastError();
                if (error < 0x60000000 || error > 0x60000003) invalidError.store(true);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    CHECK(!invalidError.load());
    System_SFile::s_lastError = previousError;
    // Two generated files exercise reverse in-place expansion, especially row 0:
    // its raw compressed-size word overlaps the expanded 64-bit offset.
    const std::array<const char*, 2> names = {"first.bin", "second.bin"};
    const std::array<std::vector<uint8_t>, 2> contents = {
        std::vector<uint8_t>{'B', 'L', 'P', '2', 0x12, 0x34, 0x56, 0x78},
        std::vector<uint8_t>(97, 0xa7)
    };
    std::vector<uint32_t> hashes(4 * 4, UINT32_MAX);
    std::vector<uint32_t> blocks;
    std::vector<uint8_t> image(44, 0);
    for (uint32_t i = 0; i < names.size(); ++i) {
        uint32_t slot = System_MPQEncrypt::Hash(names[i], 0) & 3;
        while (hashes[slot * 4 + 3] != UINT32_MAX) slot = (slot + 1) & 3;
        hashes[slot * 4] = System_MPQEncrypt::Hash(names[i], 1);
        hashes[slot * 4 + 1] = System_MPQEncrypt::Hash(names[i], 2);
        hashes[slot * 4 + 2] = 0; // Neutral locale/platform.
        hashes[slot * 4 + 3] = i;
        uint32_t size = static_cast<uint32_t>(contents[i].size());
        blocks.insert(blocks.end(), {static_cast<uint32_t>(image.size()), size, size, 0x80000000});
        image.insert(image.end(), contents[i].begin(), contents[i].end());
    }
    uint32_t hashPos = static_cast<uint32_t>(image.size());
    System_MPQEncrypt::Encrypt(hashes.data(), hashes.size() * sizeof(uint32_t), System_MPQEncrypt::Hash("(hash table)", 3));
    const auto* hashBytes = reinterpret_cast<const uint8_t*>(hashes.data());
    image.insert(image.end(), hashBytes, hashBytes + hashes.size() * sizeof(uint32_t));
    uint32_t blockPos = static_cast<uint32_t>(image.size());
    System_MPQEncrypt::Encrypt(blocks.data(), blocks.size() * sizeof(uint32_t), System_MPQEncrypt::Hash("(block table)", 3));
    const auto* blockBytes = reinterpret_cast<const uint8_t*>(blocks.data());
    image.insert(image.end(), blockBytes, blockBytes + blocks.size() * sizeof(uint32_t));
    auto put32 = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) image[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    put32(0, 0x1a51504d);
    put32(4, 44);
    put32(8, static_cast<uint32_t>(image.size()));
    put32(12, 1 | (3u << 16)); // MPQ v1, 4096-byte sectors.
    put32(16, hashPos);
    put32(20, blockPos);
    put32(24, 4);
    put32(28, names.size());

    char path[] = "/tmp/native-mpq-read-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    FILE* output = fdopen(fd, "wb");
    if (!output) { close(fd); unlink(path); return 2; }
    CHECK(std::fwrite(image.data(), 1, image.size(), output) == image.size());
    CHECK(std::fclose(output) == 0);
    Blizzard::Mopaq::SFileSetThreadingMode(2);
    System_SFile::ARCHIVEREC* archive = nullptr;
    CHECK(Blizzard::Mopaq::SFileOpenArchive(path, 0, 0, &archive));
    if (archive) {
        for (uint32_t i = 0; i < names.size(); ++i) {
            IOMpqUnit* file = nullptr;
            CHECK(Blizzard::Mopaq::SFileOpenFileEx(archive, names[i], 0, &file));
            auto entry = archive->GetBlockEntry(i);
            CHECK(entry.m_compressedSize == contents[i].size());
            CHECK(entry.m_fileSize == contents[i].size());
            CHECK(entry.m_flags == 0x80000000);
            if (file) {
                std::vector<uint8_t> data(contents[i].size());
                uint32_t read = 0;
                CHECK(Blizzard::Mopaq::SFileReadFile(file, data.data(), data.size(), &read));
                CHECK(read == data.size() && data == contents[i]);
                CHECK(Blizzard::Mopaq::SFileCloseFile(file));
            }
        }
        CHECK(Blizzard::Mopaq::SFileCloseArchive(archive));
    }
    CHECK(unlink(path) == 0);
    std::printf("MpqRead: %u failures\n", failures);
    return failures ? 1 : 0;
}
