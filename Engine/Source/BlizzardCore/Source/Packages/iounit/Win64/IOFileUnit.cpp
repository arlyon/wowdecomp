#if defined(WOWDECOMP_ARCH_64)

#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOPlatform.h"
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"
#include "Engine/Source/Storm/H/storm.h"
#if defined(_WIN32)
// OFFSET: 0x00434F20
bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    HANDLE file;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative native(path);
        file = CreateFileA(native.m_data, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    }

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint32_t head = offset & 0xFFF;
    uint32_t size = (head + count + 0xFFF) & ~0xFFFu;
    bool result = false;
    uint8_t* data = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));

    if (SetFilePointer(file, offset - head, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        DWORD read;

        if (ReadFile(file, data, size, &read, nullptr)) {
            uint32_t available = read - head;
            uint32_t amount = available >= count ? count : available;
            Blizzard::String::MemCopy(buffer, data + head, amount);

            if (bytesRead) {
                *bytesRead = amount;
            }

            result = true;
        }
    }

    SMemFree(data, __FILE__, __LINE__, 0);
    CloseHandle(file);

    return result;
}
#else
bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    return System_File::Native::ReadUnbuffered(offset, path, buffer, count, bytesRead);
}
#endif

// OFFSET: 0x00434CE0
IOUnbufferedFileUnit::IOUnbufferedFileUnit(Blizzard::File::StreamRecord* stream, bool ownsMemory) : IOFileUnit(stream, ownsMemory) {
    Blizzard::File::FileInfo* info = Blizzard::File::GetFileInfo(stream);
    this->m_size = info->m_size;
#if defined(_WIN32)
    char root[1024];
    Blizzard::String::Copy(root, info->m_path, Blizzard::File::GetRootChars(info->m_path) + 1);

    DWORD sectorsPerCluster;
    DWORD bytesPerSector;
    DWORD freeClusters;
    DWORD totalClusters;
    BOOL valid;

    {
        Blizzard::StringConversion_CodepageToUTF8::ToNative native(root);
        valid = GetDiskFreeSpaceA(native.m_data, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters);
    }
#else
    uint32_t bytesPerSector = 0;
    const bool valid = System_File::Native::GetFileAlignment(info->m_path, bytesPerSector);
#endif

#if !defined(_WIN32)
    if (!valid || !bytesPerSector || bytesPerSector > 0x100000u || ((bytesPerSector - 1) & bytesPerSector)) {
        this->m_buffer = nullptr;
        this->m_alignedBuffer = nullptr;
        this->m_alignMask = 0;
        return;
    }
    this->m_alignMask = bytesPerSector - 1;
#else
    if (!valid || ((bytesPerSector - 1) & bytesPerSector)) {
        this->m_alignMask = 0xFFFF;
    } else {
        this->m_alignMask = bytesPerSector - 1;
    }
#endif

    this->m_buffer = Blizzard::Memory::Allocate(this->m_alignMask + 0x100000);
#if !defined(_WIN32)
    if (!this->m_buffer) {
        this->m_alignedBuffer = nullptr;
        return;
    }
#endif
    this->m_alignedBuffer = reinterpret_cast<void*>(~static_cast<uintptr_t>(this->m_alignMask) & (reinterpret_cast<uintptr_t>(this->m_buffer) + this->m_alignMask));
}

#endif
