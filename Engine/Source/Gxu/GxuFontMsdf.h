#ifndef ENGINE_SOURCE_GXU_GXUFONTMSDF_H
#define ENGINE_SOURCE_GXU_GXUFONTMSDF_H

#include <cstdint>
#include <vector>
#include <freetype/freetype.h>

namespace GxuMsdf {
constexpr uint32_t AtlasSize = 256;
constexpr uint32_t Border = 4; // two-pixel outline plus antialias/filtering guard
constexpr float PixelRange = 8.0f;

// Metrics use the Classic cell; the atlas alone has Border texels on each side.
struct Glyph {
    uint32_t width = 0, height = 0, cellWidth = 0, cellHeight = 0;
    float advance = 0, bearing = 0;
    int32_t yOffset = 0;
    uint32_t yStart = 0;
    std::vector<uint8_t> rgba;
    uint32_t AtlasWidth() const { return cellWidth + 2 * Border; }
    uint32_t AtlasHeight() const { return cellHeight + 2 * Border; }
};
bool GenerateGlyph(FT_Face face, uint32_t pixelSize, uint32_t code,
    uint32_t baseline, uint32_t padding, Glyph& glyph);
// Checked copy used by the production texture latch and CPU tests.
bool PasteGlyph(const void* rgba, uint32_t bytes, uint32_t width, uint32_t height,
    uint32_t x, uint32_t y, uint8_t* atlas);
}
#endif
