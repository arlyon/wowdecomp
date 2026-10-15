#ifndef ENGINE_SOURCE_BLIZZARDCORE_OGGDECOMPRESS_H
#define ENGINE_SOURCE_BLIZZARDCORE_OGGDECOMPRESS_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/Memory/TempBuffer.h"

#if defined(_WIN32)
#define BLIZZARD_OGG_TELL_TYPE ogg_int32_t
#else
#define BLIZZARD_OGG_TELL_TYPE decltype(ov_callbacks{}.tell_func(nullptr))
#endif

typedef Blizzard::Memory::TempBuffer<uint8_t, 0x400000> OggDecompressBuffer;

struct OggMemoryStream {
    /* 0000 */ const uint8_t* m_data;
    /* 0004 */ uint32_t m_size;
    /* 0008 */ uint32_t m_position;

    size_t Read(void* buffer, size_t size, size_t count);
};

struct OggChunk {
    /* 0000 */ uint32_t m_rate;
    /* 0004 */ uint32_t m_channels;
    /* 0008 */ void* m_data;
    /* 000C */ uint32_t m_size;
};

struct OggChunkList {
    /* 0000 */ std::vector<OggChunk>* m_chunks;

    ~OggChunkList();
};

bool DecompressOgg(const void* data, uint32_t size, OggDecompressBuffer* buffer);
bool InitOggDecompression();
bool ShutdownOggDecompression();

#endif
