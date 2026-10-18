#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/Storm/H/storm.h"

// Error TLS and failure-only constructor boundaries are wrapped; I/O uses real providers.
extern "C" void __wrap__ZN8Blizzard4File12SetLastErrorEi(int32_t error) { SErrSetLastError(error); }
namespace {
unsigned failures;
void Check(bool valid, const char* text) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL %s errno=%d\n", text, errno); }
}
using Parms = System_File::Stacked::FileParms;
using FS = Blizzard::File::Filesystem;
enum Fault { None, ShortWrite, FailWrite, FailEOF, FailRead, SyncFailure, TailReadFailure, ShortRead, InvalidRead, FailClose };
Fault fault;
System_File::Stacked::FileFunc realWrite, realRead, realEOF, realClose;
uint32_t writeCalls, readCalls, closeCalls;
enum class ConstructorFault { None, QueryFailure, ZeroAlignment, NonPowerAlignment, OversizedAlignment, AllocationFailure };
ConstructorFault constructorFault;
uint32_t alignmentCalls, allocationCalls;
uint32_t maxTransfer;
bool Write(FS* fs, Parms* parms) {
    maxTransfer = std::max(maxTransfer, static_cast<uint32_t>(parms->m_amount));
    ++writeCalls;
    if (fault == FailWrite) return false; // deliberately leave amount unchanged
    if (fault == ShortWrite) parms->m_amount = 4096;
    const bool result = realWrite(fs, parms);
    return fault == SyncFailure ? false : result;
}
bool Read(FS* fs, Parms* parms) {
    ++readCalls;
    maxTransfer = std::max(maxTransfer, static_cast<uint32_t>(parms->m_amount));
    if (fault == FailRead || (fault == TailReadFailure && parms->m_pos >= 0x100000)) return false;
    if (fault == ShortRead) parms->m_amount = 0;
    if (fault == InvalidRead) { parms->m_amount += 1; return true; }
    return realRead(fs, parms);
}
bool Close(FS* fs, Parms* parms) {
    ++closeCalls;
    const bool result = realClose(fs, parms);
    return fault == FailClose ? false : result;
}
bool EOFCall(FS* fs, Parms* parms) { return fault == FailEOF ? false : realEOF(fs, parms); }
}
extern "C" bool __real__ZN11System_File6Native16GetFileAlignmentEPKcRj(const char*, uint32_t&);
extern "C" bool __wrap__ZN11System_File6Native16GetFileAlignmentEPKcRj(const char* path, uint32_t& alignment) {
    ++alignmentCalls;
    const bool result = __real__ZN11System_File6Native16GetFileAlignmentEPKcRj(path, alignment);
    if (!result) return false; // Never turn a real provider failure into success.
    switch (constructorFault) {
        case ConstructorFault::QueryFailure: return false;
        case ConstructorFault::ZeroAlignment: alignment = 0; break;
        case ConstructorFault::NonPowerAlignment: alignment = 3; break;
        case ConstructorFault::OversizedAlignment: alignment = 0x200000; break;
        default: break;
    }
    return result;
}
extern "C" void* __real__ZN8Blizzard6Memory8AllocateEj(uint32_t);
extern "C" void* __wrap__ZN8Blizzard6Memory8AllocateEj(uint32_t bytes) {
    ++allocationCalls;
    if (constructorFault == ConstructorFault::AllocationFailure) return nullptr;
    return __real__ZN8Blizzard6Memory8AllocateEj(bytes);
}
int main() {
    char directory[] = "/tmp/iounit-test-XXXXXX";
    if (!mkdtemp(directory)) return 2;
    const std::string path = std::string(directory) + "/fixture";
    // The native dispatch table is real, with explicit test-only fault seams.
    FS provider{};
    provider.m_verbs.m_open = System_File::Open;
    provider.m_verbs.m_close = Close;
    provider.m_verbs.m_getfileinfo = System_File::GetFileInfo;
    provider.m_verbs.m_readp = Read;
    provider.m_verbs.m_writep = Write;
    provider.m_verbs.m_seteof = EOFCall;
    realWrite = System_File::WriteP; realRead = System_File::ReadP; realEOF = System_File::SetEOF; realClose = System_File::Close;
    auto previous = System_File::Stacked::s_manager;
    System_File::Stacked::s_manager = &provider;
    Blizzard::File::StreamRecord* stream = nullptr;
    const std::string constructorPath = std::string(directory) + "/constructor-fixture";
    for (ConstructorFault injected : {ConstructorFault::QueryFailure, ConstructorFault::ZeroAlignment,
            ConstructorFault::NonPowerAlignment, ConstructorFault::OversizedAlignment,
            ConstructorFault::AllocationFailure}) {
        Check(Blizzard::File::Open(constructorPath.c_str(), 0x143, stream), "constructor failure fixture real open");
        if (!stream) return 2;
        const int descriptor = static_cast<int>(reinterpret_cast<uintptr_t>(stream->m_fileHandle) - 1);
        Check(fcntl(descriptor, F_GETFD) >= 0, "constructor owns live native descriptor");
        const uint32_t beforeAlignment = alignmentCalls, beforeAllocation = allocationCalls;
        constructorFault = injected;
        IOUnbufferedFileUnit unusable(stream, false);
        constructorFault = ConstructorFault::None;
        Check(alignmentCalls == beforeAlignment + 1 && allocationCalls == beforeAllocation +
              (injected == ConstructorFault::AllocationFailure ? 1u : 0u),
              "real constructor hits injected boundary and never allocates invalid alignment");
        Check(!unusable.m_buffer && !unusable.m_alignedBuffer, "constructor failure leaves both scratch pointers null");
        unsigned char byte = 0xA5;
        uint32_t got = 777;
        const uint32_t beforeRead = readCalls, beforeWrite = writeCalls, beforeClose = closeCalls;
        Check(!unusable.Read(&byte, 0, 1, &got) && got == 0 && byte == 0xA5,
              "unusable constructor read fails with zero bytes and no copy");
        Check(!unusable.Read(&byte, 0, 1, nullptr) && !unusable.Write(&byte, 0, 1),
              "unusable constructor exact read and write fail");
        Check(readCalls == beforeRead && writeCalls == beforeWrite, "unusable constructor never dispatches I/O");
        Check(unusable.Close() && !unusable.m_stream && !unusable.m_buffer && !unusable.m_alignedBuffer &&
              closeCalls == beforeClose + 1, "unusable constructor closes actual descriptor and clears ownership");
        errno = 0;
        Check(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF, "failed constructor descriptor actually closed");
        Check(!unusable.Close() && closeCalls == beforeClose + 1, "failed constructor repeated close does not redispatch");
    }
    Check(Blizzard::File::Open(path.c_str(), 0x143, stream), "SDK create O_DIRECT read/write");
    if (!stream) return 2;
    IOUnbufferedFileUnit unit(stream, false);
    Check(unit.m_alignedBuffer && unit.m_alignMask < 0x100000, "real constructor alignment");
    std::vector<unsigned char> source(0x200003, 0x56), data(source.size() + 2, 0xA5);
    Check(unit.Write(source.data() + 1, 0, source.size() - 1), "bounded unaligned multi-window write");
    const int64_t logical = source.size() - 1;
    Check(unit.m_size == logical, "logical size excludes padding");
    uint32_t got = 999;
    Check(unit.Read(data.data() + 1, 0, logical + 1, &got) && got == logical + 1 && data[logical + 1] == 0,
          "physical tail padding deterministic before close");
    Check(data.front() == 0xA5 && data.back() == 0xA5, "read caller canaries");
    unsigned char patch[] = {1, 2, 3};
    Check(unit.Write(patch, 7, 3), "unaligned offset RMW");
    unsigned char neighbors[8]{};
    Check(unit.Read(neighbors, 5, 8, nullptr) && neighbors[0] == 0x56 && neighbors[1] == 0x56 &&
          neighbors[2] == 1 && neighbors[4] == 3 && neighbors[5] == 0x56, "existing neighbors preserved");
    for (Fault injected : {TailReadFailure, ShortRead}) {
        fault = injected;
        const uint32_t before = writeCalls;
        Check(!unit.Write(source.data(), 0, 0x100001) && writeCalls == before,
              "entire-request preflight rejects failed/short tail read before writes");
        fault = None;
        Check(unit.Read(neighbors, 5, 8, nullptr) && neighbors[2] == 1 && neighbors[4] == 3,
              "failed preflight leaves real prefix fixture unchanged");
    }
    fault = InvalidRead; got = 777;
    Check(!unit.Read(data.data() + 1, 0, 8, &got) && got == 0, "invalid provider count rejected");
    fault = FailRead; got = 777;
    Check(!unit.Read(data.data() + 1, 0, 8, &got) && got == 0, "read failure clears count");
    fault = FailWrite;
    Check(!unit.Write(patch, logical + 100, 3) && unit.m_size == logical, "false write unchanged count not accepted");
    fault = FailEOF;
    Check(!unit.SetFileSize(logical + 99) && unit.m_size == logical, "failed EOF preserves logical size");
    fault = None;
    Check(!unit.Read(data.data(), -1, 1, &got) && got == 0 && !unit.Write(patch, INT64_MAX, 3), "invalid offsets rejected");
    Check(unit.Read(nullptr, 0, 0, &got) && got == 0 && unit.Write(nullptr, 0, 0), "zero length no-op");
    Check(unit.Close() && !unit.Close(), "truncate close and repeat close safe");
    Check(std::filesystem::file_size(path) == static_cast<uint64_t>(logical), "close physical size matches logical");
    Check(Blizzard::File::Open(path.c_str(), 0x1041, stream), "reopen real read-only direct");
    IOUnbufferedFileUnit reader(stream, false);
    std::fill(data.begin(), data.end(), 0xA5);
    Check(reader.Read(data.data() + 1, logical - 5, 20, &got) && got == 5 && data[6] == 0xA5, "short EOF no stale bytes");
    Check(!reader.Read(data.data() + 1, logical, 20, &got) && got == 0 && data[6] == 0xA5, "EOF no stale copy");
    Check(!reader.Read(data.data(), logical - 5, 20, nullptr), "exact short read fails");
    Check(!reader.Write(patch, 0, 3), "read-only descriptor never widened");
    Check(reader.Close(), "read-only close");
    Check(Blizzard::File::Open(path.c_str(), 0x1042, stream), "write-only open");
    IOUnbufferedFileUnit writer(stream, false);
    Check(!writer.Write(source.data(), 0, 0x100001), "whole-write preflight rejects final tail preservation");
    Check(writer.Write(source.data(), 0x100000, 4096), "write-only fully covered sector needs no read");
    Check(writer.Close(), "write-only close");
    Check(Blizzard::File::Open(path.c_str(), 0x1043, stream), "wide fixture reopen");
    IOUnbufferedFileUnit wide(stream, false);
    Check(wide.Read(neighbors, 5, 8, nullptr) && neighbors[2] == 1, "preflight caused no earlier write");
    const int64_t high = (int64_t{1} << 33) + 17;
    Check(wide.Write(patch, high, 3) && wide.Read(neighbors, high, 3, &got) && got == 3 && neighbors[0] == 1,
          "sparse wide offsets preserve 64 bits");
    fault = ShortWrite;
    const int64_t oldSize = wide.m_size;
    Check(!wide.Write(source.data(), oldSize + 8192, 8192) && wide.m_size > oldSize && wide.m_size < oldSize + 16384,
          "short successful provider write accounts only known accepted prefix");
    fault = SyncFailure;
    const int64_t acceptedSize = wide.m_size;
    Check(!wide.Write(patch, acceptedSize + 16384, 3) && wide.m_size == acceptedSize, "post-write failure no size claim or rollback guarantee");
    fault = None;
    Check(wide.Read(neighbors, acceptedSize + 16384, 3, &got) && got == 3 &&
          std::memcmp(neighbors, patch, 3) == 0,
          "failed post-write result can still mutate actual descriptor data");
    fault = FailEOF;
    const uint32_t beforeClose = closeCalls;
    Check(!wide.Close() && !wide.m_stream && !wide.m_buffer && closeCalls == beforeClose + 1,
          "failed truncate still closes/frees");
    fault = None;
    Check(Blizzard::File::Open(path.c_str(), 0x1041, stream), "close-error fixture open");
    IOUnbufferedFileUnit closeError(stream, false);
    fault = FailClose;
    Check(!closeError.Close() && !closeError.m_stream && !closeError.m_buffer && !closeError.Close(),
          "provider close error still frees and clears ownership");
    fault = None;
    Check(maxTransfer <= 0x100000, "all transfers bounded by one MiB");
    System_File::Stacked::s_manager = previous;
    std::filesystem::remove_all(directory);
    std::printf("IOUnitTest: %u failures\n", failures);
    return failures ? 1 : 0;
}
