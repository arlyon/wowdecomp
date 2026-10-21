#ifndef WOW_SOURCE_WORLDCLIENT_FARTERRAIN_H
#define WOW_SOURCE_WORLDCLIENT_FARTERRAIN_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/BLPFile/Types.h"

namespace FarTerrain {
constexpr unsigned TextureCapacity = 256;
constexpr unsigned RequestsPerFrame = 8;
constexpr unsigned InFlightLimit = 8;
constexpr unsigned PendingFrameLimit = 600;
constexpr unsigned MaxTextureFileBytes = 512 * 1024;
constexpr unsigned MaxTextureDimension = 256;
constexpr float TileSize = 533.33331f;

inline uint32_t Word(const char *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline bool Range(size_t size, size_t offset, size_t bytes) {
    return offset <= size && bytes <= size - offset;
}
// Validate all decoder-accessible levels before issuing the asynchronous body read.
inline bool ValidTexture(const BLPHeader &h, size_t bytes) {
    if (bytes < sizeof(h) || bytes > MaxTextureFileBytes || h.m_magic != 0x32504c42 || h.m_formatVersion != 1 ||
        !h.m_width || !h.m_height || h.m_width > MaxTextureDimension || h.m_height > MaxTextureDimension ||
        (h.m_width & (h.m_width - 1)) || (h.m_height & (h.m_height - 1)) ||
        (h.m_colorEncoding != 1 && h.m_colorEncoding != 2 && h.m_colorEncoding != 3) ||
        (h.m_alphaSize != 0 && h.m_alphaSize != 1 && h.m_alphaSize != 4 && h.m_alphaSize != 8))
        return false;
    if (h.m_colorEncoding == 3 && h.m_preferredFormat != 2)
        return false;
    if (h.m_colorEncoding == 2 && h.m_preferredFormat != 0 && h.m_preferredFormat != 1 && h.m_preferredFormat != 7)
        return false;
    unsigned width = h.m_width, height = h.m_height;
    bool ended = false;
    for (unsigned level = 0; level < 16; ++level) {
        if (!h.m_mipSizes[level]) {
            ended = true;
            continue;
        }
        if (ended || level > 8 || h.m_mipOffsets[level] < 148 ||
            !Range(bytes, h.m_mipOffsets[level], h.m_mipSizes[level]))
            return false;
        unsigned required = width * height * 4;
        if (h.m_colorEncoding == 1) {
            if (h.m_mipOffsets[level] < sizeof(h))
                return false;
            required = width * height + (width * height * h.m_alphaSize + 7) / 8;
        }
        if (h.m_colorEncoding == 2)
            required = ((width + 3) / 4) * ((height + 3) / 4) * (h.m_preferredFormat == 0 ? 8 : 16);
        if (h.m_mipSizes[level] < required)
            return false;
        if (width == 1 && height == 1) {
            for (unsigned i = level + 1; i < 16; ++i)
                if (h.m_mipSizes[i])
                    return false;
        }
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    unsigned levels = 1;
    for (unsigned size = std::max(h.m_width, h.m_height); size > 1; size /= 2)
        ++levels;
    if ((h.m_hasMips & 15) && !h.m_mipSizes[levels - 1])
        return false;
    return h.m_mipSizes[0] != 0;
}
inline std::string Key(std::string_view text) {
    std::string result(text);
    for (char &c : result) {
        if (c == '/')
            c = '\\';
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return result;
}
// Minimap source axes are mapX (decreasing world Y), mapY (decreasing world X).
inline C2Vector UV(unsigned vertex) {
    if (vertex < 289)
        return C2Vector(float(vertex % 17) / 16.f, float(vertex / 17) / 16.f);
    vertex -= 289;
    return C2Vector((float(vertex % 16) + .5f) / 16.f, (float(vertex / 16) + .5f) / 16.f);
}
inline C3Vector Normal(const int16_t *heights, unsigned vertex) {
    const auto uv = UV(vertex);
    const int col = std::clamp(int(std::lround(uv.x * 16)), 0, 16);
    const int row = std::clamp(int(std::lround(uv.y * 16)), 0, 16);
    const int l = std::max(0, col - 1), r = std::min(16, col + 1);
    const int t = std::max(0, row - 1), b = std::min(16, row + 1);
    const float dx = float(heights[b * 17 + col] - heights[t * 17 + col]) / ((b - t) * 33.333332f);
    const float dy = float(heights[row * 17 + r] - heights[row * 17 + l]) / ((r - l) * 33.333332f);
    const float length = std::sqrt(dx * dx + dy * dy + 1.f);
    return C3Vector(dx / length, dy / length, 1.f / length);
}
inline void Vertex(CGxVertexPCT &out, const C3Vector &topLeft, const int16_t *heights, unsigned index,
                   const C3Vector &direction, CImVector ambient, CImVector diffuse) {
    out.tc = UV(index);
    out.p = C3Vector(topLeft.x - out.tc.y * TileSize, topLeft.y - out.tc.x * TileSize, float(heights[index]));
    const auto n = Normal(heights, index);
    const float light = std::max(0.f, n.x * direction.x + n.y * direction.y + n.z * direction.z);
    const auto channel = [light](unsigned a, unsigned d) {
        return uint8_t(std::min(255.f, float(a) + float(d) * light));
    };
    out.c = CImVector(channel(ambient.b, diffuse.b), channel(ambient.g, diffuse.g), channel(ambient.r, diffuse.r), 255);
}
} // namespace FarTerrain
#endif
