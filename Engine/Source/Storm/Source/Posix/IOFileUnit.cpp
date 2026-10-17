#include <algorithm>
#include <cerrno>
#include <climits>
#include <new>
#include "Engine/Source/Storm/Source/IOUnit/IOFileUnit.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"

namespace Storm {
namespace {
bool Fail(int error) {
    errno = error;
    SErrSetLastError(error);
    return false;
}

// These share bits are compatibility hints, not Windows mandatory-sharing locks.
// Only data access, the five creation dispositions, NORMAL and NO_BUFFERING
// are supported. In particular, do not silently accept delete-on-close/overlapped.
bool Translate(const IOFileParam& param, int32_t& mode) {
    if (!param.m_name || !*param.m_name) return Fail(EINVAL);
    if ((param.m_access & ~0xC0000000u) || !param.m_access ||
        (param.m_share & ~3u) || (param.m_flags & ~0x20000080u)) return Fail(ENOTSUP);
    mode = ((param.m_access & 0x80000000u) ? 1 : 0) |
           ((param.m_access & 0x40000000u) ? 2 : 0);
    switch (param.m_creation) {
        case 1: mode |= 0x800; break; // CREATE_NEW
        case 2: mode |= 0x100; break; // CREATE_ALWAYS
        case 3: mode |= 0x1000; break; // OPEN_EXISTING
        case 4: break; // OPEN_ALWAYS
        case 5: mode |= 0x1000; break; // TRUNCATE_EXISTING, after open
        default: return Fail(EINVAL);
    }
    if ((param.m_creation == 2 || param.m_creation == 5) && !(mode & 2)) return Fail(EACCES);
    if (param.m_flags & 0x20000000u) mode |= 0x40;
    return true;
}

class NativeFileUnit final : public IOUnit {
    Blizzard::File::StreamRecord* m_stream;
    ::IOUnbufferedFileUnit* m_direct = nullptr;
public:
    explicit NativeFileUnit(Blizzard::File::StreamRecord* stream) : m_stream(stream) {}
    ~NativeFileUnit() override {
        if (m_direct) {
            m_direct->Close();
            delete m_direct;
        } else {
            Blizzard::File::Close(m_stream);
        }
    }
    bool EnableDirect() {
        m_direct = new (std::nothrow) ::IOUnbufferedFileUnit(m_stream, false);
        if (!m_direct || !m_direct->m_alignedBuffer) return Fail(ENOMEM);
        return true;
    }
    bool Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) override {
        if (bytesRead) *bytesRead = 0;
        if (!Valid(buffer, offset, count)) return false;
        if (m_direct) return m_direct->Read(buffer, offset, count, bytesRead);
        uint32_t total = 0;
        while (total < count) {
            const int32_t request = static_cast<int32_t>(std::min(count - total, uint32_t{INT_MAX}));
            int32_t amount = request;
            if (!Blizzard::File::Read(m_stream, static_cast<unsigned char*>(buffer) + total, offset + total, &amount)) return false;
            if (amount < 0 || amount > request) return Fail(EIO);
            total += amount;
            if (bytesRead) *bytesRead = total;
            if (amount != request) break;
        }
        return bytesRead ? (count == 0 || total != 0) : total == count;
    }
    bool Write(const void* buffer, int64_t offset, uint32_t count) override {
        if (!Valid(buffer, offset, count)) return false;
        if (m_direct) return m_direct->Write(buffer, offset, count);
        uint32_t total = 0;
        while (total < count) {
            const int32_t request = static_cast<int32_t>(std::min(count - total, uint32_t{INT_MAX}));
            int32_t amount = request;
            if (!Blizzard::File::Write(m_stream, static_cast<const unsigned char*>(buffer) + total, offset + total, &amount)) return false;
            if (amount != request) return Fail(EIO);
            total += amount;
        }
        return true;
    }
    bool Flush() override {
        System_File::Stacked::FileParms parms{};
        parms.m_stream = m_stream;
        parms.m_verb = offsetof(Blizzard::File::Filesystem, m_verbs.m_flush);
        auto manager = System_File::Stacked::s_manager;
        if (!manager || !manager->m_verbs.m_flush) return Fail(ENOTSUP);
        return manager->m_verbs.m_flush(manager, &parms);
    }
    bool GetFileSize(int64_t* size) override {
        if (!size) return Fail(EINVAL);
        if (m_direct) return m_direct->GetFileSize(size);
        Blizzard::File::FileInfo info{};
        if (!Blizzard::File::GetFileInfo(m_stream, &info)) { *size = 0; return false; }
        *size = info.m_size;
        return true;
    }
    bool SetFileSize(int64_t size) override {
        if (size < 0) return Fail(EINVAL);
        return m_direct ? m_direct->SetFileSize(size) : Blizzard::File::SetEOF(m_stream, size, 0);
    }
    void* Win32Handle() override { Fail(ENOTSUP); return nullptr; }
private:
    static bool Valid(const void* buffer, int64_t offset, uint32_t count) {
        return (offset < 0 || offset > INT64_MAX - count || (count && !buffer)) ? Fail(EINVAL) : true;
    }
};
}

IOUnit::IOUnit() : m_refcount(nullptr), m_count(1) {}
IOUnit::~IOUnit() {}
IORefcount::~IORefcount() {}

void IORefcount::Release() {
    IORefcount* refcount = this;
    do {
        IORefcount* next = refcount->m_refcount;
        if (--refcount->m_count == 0) delete refcount;
        refcount = next;
    } while (refcount);
}

void IOUnit::Release() {
    IORefcount* refcount = m_refcount;
    if (--m_count == 0) delete this;
    if (refcount) refcount->Release();
}

IOUnit* IOFileUnit::Create(const IOFileParam& param, IOUnit* next) {
    if (next) { Fail(ENOTSUP); return nullptr; }
    int32_t mode;
    if (!Translate(param, mode)) return nullptr;
    Blizzard::File::StreamRecord* stream = nullptr;
    if (!Blizzard::File::Open(param.m_name, mode, stream)) return nullptr;
    if (param.m_creation == 5 && !Blizzard::File::SetEOF(stream, 0, 0)) {
        Blizzard::File::Close(stream);
        return nullptr;
    }
    auto unit = new (std::nothrow) NativeFileUnit(stream);
    if (!unit) { Blizzard::File::Close(stream); Fail(ENOMEM); return nullptr; }
    if ((mode & 0x40) && !unit->EnableDirect()) { delete unit; return nullptr; }
    return unit;
}
}
