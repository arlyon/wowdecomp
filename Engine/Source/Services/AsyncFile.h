#ifndef ENGINE_SOURCE_SERVICES_ASYNCFILE_H
#define ENGINE_SOURCE_SERVICES_ASYNCFILE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Engine/Source/Services/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/Source/SThread.h"

// Worker-only exact deferred read algorithm. Reader supplies Size() and
// Read(offset,buffer,count,bytesRead) on its already-open immutable source.
// Kept independent of SFile for deterministic large-file/range/error fixtures.
template<class Reader> void AsyncFileReadFilenameBytes(CAsyncObject& object, Reader& reader) {
    const uint32_t size=object.m_fileBytes=reader.Size();
    auto& result=object.m_filenameResult;
    object.m_readSucceeded=false;object.m_bytesRead=0;
    const bool range=object.m_filenameMode==AsyncFilenameMode::Range;
    const uint64_t offset=range?object.m_readOffset:0;
    const uint32_t count=range?object.m_size:size;
    result=AsyncFilenameResult::InvalidRange;
    if((!range && (!size || size>object.m_size)) || offset>size || count>size-offset) {
        if(range && offset==size && count) result=AsyncFilenameResult::EndOfFile;
        return;
    }
    if((object.m_expectedFileBytes && size!=object.m_expectedFileBytes) ||
        object.m_expectedPrefixBytes>1172 || (object.m_expectedPrefixBytes && !object.m_expectedPrefix)) {
        result=AsyncFilenameResult::SourceChanged;return;
    }
    uint8_t prefix[1172];uint32_t got=0;
    const uint32_t n=object.m_expectedPrefixBytes;
    if(n && (!reader.Read(0,prefix,n,got) || got!=n || std::memcmp(prefix,object.m_expectedPrefix,n))) {
        result=AsyncFilenameResult::SourceChanged;return;
    }
    object.m_readSucceeded=!count || (reader.Read(uint32_t(offset),object.m_buffer,count,object.m_bytesRead) && object.m_bytesRead==count);
    result=object.m_readSucceeded?AsyncFilenameResult::Complete:AsyncFilenameResult::ReadError;
}

class CAsyncQueue : public TSLinkedNode<CAsyncQueue> {
    public:
    /* 0008 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_readList;
    /* 0014 */ TSExplicitList<CAsyncObject, offsetof(CAsyncObject, m_link)> m_staleList;
    /* 0020 */ int32_t m_frameLimited;

    ~CAsyncQueue();
};

class CAsyncThread : public TSLinkedNode<CAsyncThread> {
    public:
    /* 0008 */ SThread m_thread;
    /* 000C */ CAsyncQueue* m_queue;
    /* 0010 */ CAsyncObject* m_currentObject;
};

class AsyncFile {
    public:
    static int32_t Cancel(CAsyncObject* object, void (*callback)(CAsyncObject*));
    static void EnterQueueLock();
    static void LeaveQueueLock();
};

void AsyncFileReadAddPollHandler(void (*handler)());
void AsyncFileReadAddStatusHandler(int32_t (*handler)());
CAsyncObject* AsyncFileReadAllocObject();
CAsyncQueue* AsyncFileReadCreateQueue();
void AsyncFileReadCreateThread(CAsyncQueue* queue, const char* name);
void AsyncFileReadDestroy();
void AsyncFileReadDestroyObject(CAsyncObject* object);
void AsyncFileReadIncreasePriority(CAsyncObject* object);
void AsyncFileReadIngameProgressCallback(void (*startCallback)(), void (*progressCallback)(float, int32_t));
void AsyncFileReadInitialize(uint32_t threadSleep, uint32_t handlerTimeout);
bool AsyncFileReadIsReading();
void AsyncFileReadLinkObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadLinkStaleObject(CAsyncObject* object, int32_t urgent);
void AsyncFileReadObject(CAsyncObject* object, int32_t urgent);
// Caller owns a precharged m_buffer of m_size bytes. No open/read on caller thread.
// Completion reports exact m_bytesRead/m_readSucceeded; oversize files are refused.
void AsyncFileReadFilename(CAsyncObject* object, const char* filename, uint32_t flags);
// Range reads are exact, including zero-length metadata reads. Worker reports
// file size; a prefix+size expectation prevents mixing reopened source headers.
// Prefix <=1172 bytes, immutable until callback; buffer is precharged by caller.
void AsyncFileReadFilenameRange(CAsyncObject* object, const char* filename, uint32_t flags,
    uint64_t offset, uint32_t expectedFileBytes = 0,
    const uint8_t* expectedPrefix = nullptr, uint32_t expectedPrefixBytes = 0);
int32_t AsyncFileReadPollHandler(const void* data, void* param);
void AsyncFileReadSetProgressCallback(void (*callback)(float, void*), void* param);
uint32_t STORMAPI AsyncFileReadThread(void* param);
void AsyncFileReadUnloadPostList();
void AsyncFileReadWait(CAsyncObject* object);
void AsyncFileReadWaitAll();
void DefaultAsyncObjectCleanupCallback(CAsyncObject* object);
int32_t NetQueuePromoteLocalFiles(const void* data, void* param);

#endif
