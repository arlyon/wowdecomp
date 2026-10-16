#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>
#include "Engine/Source/Storm/Source/IOUnit/IOFileUnit.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"

// Use real descriptor providers; avoid initializing unrelated client TLS/services.
extern "C" void __wrap__ZN8Blizzard4File12SetLastErrorEi(int32_t error) { SErrSetLastError(error); }
namespace {
unsigned failures, closes;
bool failRead, shortWrite, failOpen, failEOF, failClose;
void Check(bool valid, const char* text) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL %s errno=%d\n", text, errno); }
}
bool Close(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* p) {
    ++closes;
    const bool result = System_File::Close(fs, p);
    return failClose ? false : result;
}
bool Open(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* p) {
    return failOpen ? false : System_File::Open(fs, p);
}
bool SetEOF(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* p) {
    return failEOF ? false : System_File::SetEOF(fs, p);
}
bool Read(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* p) {
    if (failRead) return false;
    return System_File::ReadP(fs, p);
}
bool Write(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* p) {
    if (shortWrite && p->m_amount) --p->m_amount;
    return System_File::WriteP(fs, p);
}
struct Ref final : Storm::IORefcount {
    unsigned& destroyed;
    Ref(unsigned& value, Storm::IORefcount* next, int count) : destroyed(value) {
        m_refcount = next; m_count = count;
    }
    ~Ref() override { ++destroyed; }
};
}
int main() {
    char directory[] = "/tmp/storm-archive-io-XXXXXX";
    if (!mkdtemp(directory)) return 2;
    const std::string path = std::string(directory) + "/generated.mpq";
    Blizzard::File::Filesystem provider{};
    provider.m_verbs.m_open = Open;
    provider.m_verbs.m_close = Close;
    provider.m_verbs.m_getfileinfo = System_File::GetFileInfo;
    provider.m_verbs.m_readp = Read;
    provider.m_verbs.m_writep = Write;
    provider.m_verbs.m_seteof = SetEOF;
    provider.m_verbs.m_flush = System_File::Flush;
    const auto previous = System_File::Stacked::s_manager;
    System_File::Stacked::s_manager = &provider;
    // Minimal empty MPQ v0 layout: 32-byte header, empty hash/block tables.
    const unsigned char header[32] = {'M','P','Q',0x1A,32,0,0,0,32,0,0,0,0,0,3,0,32,0,0,0,32,0,0,0};
    for (uint32_t flags : {0u, 0x20000080u}) {
        Storm::IOFileParam p{path.c_str(), 0xC0000000u, 0, 2, flags};
        auto unit = Storm::IOFileUnit::Create(p, nullptr);
        Check(unit != nullptr, "native factory buffered/direct");
        if (!unit) continue;
        Check(unit->m_count == 1 && !unit->m_refcount, "normal base constructor");
        Check(unit->Write(header, 0, sizeof(header)), "generated MPQ header write");
        int64_t size = -1;
        Check(unit->GetFileSize(&size) && size == 32, "logical size");
        unsigned char data[40]; std::memset(data, 0xA5, sizeof(data));
        uint32_t got = 999;
        Check(unit->Read(data + 1, 1, 7, &got) && got == 7 && data[0] == 0xA5 && data[8] == 0xA5 &&
              !std::memcmp(data + 1, header + 1, 7), "unaligned positioned read and canaries");
        Check(unit->Read(data, 0, 4, nullptr) && !std::memcmp(data, header, 4), "seek backwards");
        Check(!unit->Read(data, -1, 1, &got) && got == 0 && !unit->Write(header, INT64_MAX, 2), "invalid ranges");
        Check(unit->Read(nullptr, 0, 0, &got) && !got && unit->Write(nullptr, 0, 0), "zero transfers");
        failRead = true; got = 999;
        Check(!unit->Read(data, 0, 4, &got) && got == 0, "provider read error clears count");
        failRead = false;
        Check(!unit->Win32Handle() && errno == ENOTSUP, "no fabricated Windows handle");
        const int64_t high = (int64_t{1} << 33) + 17;
        Check(unit->Write(header, high, sizeof(header)), "sparse offset beyond 32 bits");
        Check(unit->Read(data, high, 32, nullptr) && !std::memcmp(data, header, 32), "wide positioned read");
        Check(unit->SetFileSize(32) && unit->Flush(), "truncate and flush");
        Check(!unit->SetFileSize(-1), "negative EOF rejected");
        auto parent = Storm::IOFileUnit::Create(p, unit);
        Check(!parent && unit->m_count == 1, "non-null next rejected without consuming ownership");
        unsigned destroyed = 0;
        auto tail = new Ref(destroyed, nullptr, 2);
        auto head = new Ref(destroyed, tail, 2);
        unit->m_refcount = head; ++unit->m_count;
        const unsigned before = closes;
        unit->Release();
        Check(closes == before && unit->m_count == 1 && head->m_count == 1 && tail->m_count == 1,
              "nonfinal unit release releases each refchain link");
        unit->Release();
        Check(closes == before + 1 && destroyed == 2, "final release closes once and releases saved chain");
        Check(std::filesystem::file_size(path) == 32, "close trims direct padding using same descriptor");
        p.m_access = 0x80000000u; p.m_creation = 3; p.m_share = 1;
        unit = Storm::IOFileUnit::Create(p, nullptr);
        Check(unit != nullptr, "read-only open");
        if (unit) {
            got = 999; std::memset(data, 0xA5, sizeof(data));
            Check(unit->Read(data, 30, 8, &got) && got == 2 && data[2] == 0xA5, "short EOF no stale bytes");
            Check(!unit->Read(data, 30, 8, nullptr), "exact short read fails");
            Check(!unit->Read(data, 32, 1, &got) && got == 0, "EOF fails with zero count");
            Check(!unit->Write(header, 0, 32) && !unit->SetFileSize(0), "read-only rights never widened");
            unit->Release();
        }
        p.m_access = 0x40000000u;
        unit = Storm::IOFileUnit::Create(p, nullptr);
        Check(unit != nullptr, "write-only open");
        if (unit) {
            Check(!unit->Read(data, 0, 1, &got) && !got, "write-only read rejected");
            if (!flags) {
                shortWrite = true;
                Check(!unit->Write(header, 0, 32), "short successful provider write is not full success");
                shortWrite = false;
            } else {
                Check(!unit->Write(header, 1, 3), "direct write-only RMW fails without widening access");
            }
            unit->Release();
        }
    }
    Storm::IOFileParam p{path.c_str(), 0xC0000000u, 0, 1, 0};
    Check(!Storm::IOFileUnit::Create(p, nullptr), "CREATE_NEW existing failure");
    const unsigned beforeFailure = closes;
    failOpen = true;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && closes == beforeFailure, "failed open owns no descriptor");
    failOpen = false;
    p.m_creation = 5;
    failEOF = true;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && closes == beforeFailure + 1, "failed factory truncate closes opened descriptor");
    failEOF = false;
    auto unit = Storm::IOFileUnit::Create(p, nullptr);
    Check(unit != nullptr, "TRUNCATE_EXISTING"); if (unit) unit->Release();
    Check(std::filesystem::file_size(path) == 0, "truncate existing physical size");
    std::filesystem::remove(path);
    Check(!Storm::IOFileUnit::Create(p, nullptr), "truncate missing fails without creating");
    p.m_creation = 3;
    Check(!Storm::IOFileUnit::Create(p, nullptr), "open missing fails");
    p.m_creation = 4;
    unit = Storm::IOFileUnit::Create(p, nullptr);
    Check(unit != nullptr, "OPEN_ALWAYS creates");
    if (unit) {
        const unsigned before = closes;
        failClose = true;
        unit->Release();
        failClose = false;
        Check(closes == before + 1, "release attempts real close once even when provider reports failure");
    }
    for (uint32_t flags : {0x40000000u, 0x04000000u, 1u}) {
        p.m_flags = flags;
        Check(!Storm::IOFileUnit::Create(p, nullptr) && errno == ENOTSUP, "unsupported flags explicit failure");
    }
    p.m_flags = 0;
    p.m_creation = 4;
    unit = Storm::IOFileUnit::Create(p, nullptr);
    Check(unit && unit->Write(header, 0, 32), "OPEN_ALWAYS existing writable");
    if (unit) unit->Release();
    unit = Storm::IOFileUnit::Create(p, nullptr);
    int64_t preservedSize = -1;
    Check(unit && unit->GetFileSize(&preservedSize) && preservedSize == 32, "OPEN_ALWAYS does not truncate");
    if (unit) unit->Release();
    std::filesystem::remove(path);
    p.m_creation = 1;
    unit = Storm::IOFileUnit::Create(p, nullptr);
    Check(unit != nullptr, "CREATE_NEW missing succeeds");
    if (unit) unit->Release();
    p.m_creation = 6;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && errno == EINVAL, "invalid disposition rejected");
    p.m_creation = 2; p.m_access = 0x80000000u;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && errno == EACCES, "read-only truncation rejected before open");
    p.m_creation = 3; p.m_access = 0xC0000000u;
    p.m_share = 4;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && errno == ENOTSUP, "delete-sharing unsupported");
    p.m_share = 0; p.m_access = 0x10000000u;
    Check(!Storm::IOFileUnit::Create(p, nullptr) && errno == ENOTSUP, "unknown rights unsupported");
    System_File::Stacked::s_manager = previous;
    std::filesystem::remove_all(directory);
    std::printf("StormArchiveIoTest: %u failures\n", failures);
    return failures ? 1 : 0;
}
