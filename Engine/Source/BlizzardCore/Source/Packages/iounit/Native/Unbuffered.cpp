// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOPlatform.h"
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Unicode/Unicode.h"
#include "Engine/Source/Storm/H/storm.h"

bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    return System_File::Native::ReadUnbuffered(offset, path, buffer, count, bytesRead);
}

// OFFSET: 0x00434CE0
IOUnbufferedFileUnit::IOUnbufferedFileUnit(Blizzard::File::StreamRecord* stream, bool ownsMemory) : IOFileUnit(stream, ownsMemory) {
    Blizzard::File::FileInfo* info = Blizzard::File::GetFileInfo(stream);
    this->m_size = info->m_size;

    uint32_t bytesPerSector = 0;
    const bool valid = System_File::Native::GetFileAlignment(info->m_path, bytesPerSector);

    if (!valid || !bytesPerSector || bytesPerSector > 0x100000u || ((bytesPerSector - 1) & bytesPerSector)) {
        this->m_buffer = nullptr;
        this->m_alignedBuffer = nullptr;
        this->m_alignMask = 0;
        return;
    }
    this->m_alignMask = bytesPerSector - 1;

    this->m_buffer = Blizzard::Memory::Allocate(this->m_alignMask + 0x100000);

    if (!this->m_buffer) {
        this->m_alignedBuffer = nullptr;
        return;
    }

    this->m_alignedBuffer = reinterpret_cast<void*>(~static_cast<uintptr_t>(this->m_alignMask) & (reinterpret_cast<uintptr_t>(this->m_buffer) + this->m_alignMask));
}
