#include "WoW/Source/WorldClient/FarTerrain.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace FarTerrain;
    assert(Key("Azeroth/map32_49.BLP") == "azeroth\\map32_49.blp");
    assert(Range(10, 10, 0) && !Range(10, 11, 0) && !Range(10, 9, 2));
    int16_t heights[545]{};
    for (unsigned i = 0; i < 545; ++i)
        heights[i] = int16_t(UV(i).y * 1600);
    for (unsigned i = 0; i < 545; ++i) {
        CGxVertexPCT v;
        Vertex(v, C3Vector(-10000, -11000, 0), heights, i, C3Vector(0, 0, 1), CImVector(30, 20, 10, 255),
               CImVector(0, 0, 0, 255));
        assert(v.tc.x >= 0 && v.tc.x <= 1 && v.tc.y >= 0 && v.tc.y <= 1);
        assert(v.p.x <= -10000 && v.p.x >= -10000 - TileSize && v.p.y <= -11000 && v.p.y >= -11000 - TileSize);
        assert(v.p.z == heights[i] && v.c.r == 10 && v.c.b == 30 && v.c.a == 255);
        const auto normal = Normal(heights, i);
        assert(normal.x > 0 && normal.z > 0 && std::abs(normal.y) < .001f);
    }
    assert(UV(0).x == 0 && UV(0).y == 0 && UV(288).x == 1 && UV(288).y == 1);
    assert(UV(289).x == .03125f && UV(544).y == .96875f);
    BLPHeader h{};
    h.m_magic = 0x32504c42;
    h.m_formatVersion = 1;
    h.m_width = 256;
    h.m_height = 256;
    h.m_colorEncoding = 3;
    h.m_preferredFormat = 2;
    h.m_alphaSize = 8;
    h.m_mipOffsets[0] = sizeof(h);
    h.m_mipSizes[0] = 256 * 256 * 4;
    const unsigned bytes = sizeof(h) + h.m_mipSizes[0];
    assert(ValidTexture(h, bytes));
    h.m_hasMips = 1;
    assert(!ValidTexture(h, bytes));
    h.m_hasMips = 0;
    h.m_mipSizes[0] = 1;
    assert(!ValidTexture(h, bytes));
    h.m_mipSizes[0] = 256 * 256 * 4;
    h.m_width = 257;
    assert(!ValidTexture(h, bytes));
    h.m_width = 256;
    h.m_preferredFormat = 1;
    assert(!ValidTexture(h, bytes));
    h.m_preferredFormat = 2;
    h.m_mipOffsets[0] = bytes;
    assert(!ValidTexture(h, bytes));
    h.m_mipOffsets[0] = sizeof(h);
    h.m_mipSizes[15] = 16;
    assert(!ValidTexture(h, bytes));
    h.m_mipSizes[15] = 0;
    h.m_colorEncoding = 2;
    h.m_preferredFormat = 0;
    h.m_alphaSize = 0;
    h.m_mipSizes[0] = 32768;
    assert(ValidTexture(h, sizeof(h) + 32768));
    h.m_preferredFormat = 7;
    assert(!ValidTexture(h, sizeof(h) + 32768));
    h.m_mipSizes[0] = 65536;
    assert(ValidTexture(h, sizeof(h) + 65536));
    h.m_colorEncoding = 1;
    h.m_preferredFormat = 8;
    h.m_alphaSize = 4;
    h.m_mipSizes[0] = 98304;
    assert(ValidTexture(h, sizeof(h) + 98304));
    std::cout << "FarTerrain: world/UV axes, negative coordinates, heights/normals, day/night colour channels, bounded "
                 "BLP layouts passed\n";
}
