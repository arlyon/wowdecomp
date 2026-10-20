#include "Engine/Source/Gxu/GxuFontMsdf.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <freetype/ftoutln.h>
#include <msdfgen.h>

namespace GxuMsdf {
namespace {
msdfgen::Point2 Point(const FT_Vector* p) { return {p->x / 64.0, p->y / 64.0}; }
struct Outline {
    msdfgen::Shape shape;
    msdfgen::Point2 first, last;
    void Close() {
        if (!shape.contours.empty() && last != first)
            shape.contours.back().addEdge(msdfgen::EdgeHolder(last, first));
    }
};
int Move(FT_Vector* p, void* user) {
    auto& o = *static_cast<Outline*>(user);
    o.Close(); o.shape.addContour(); o.first = o.last = Point(p); return 0;
}
int Line(FT_Vector* p, void* user) {
    auto& o = *static_cast<Outline*>(user);
    if (o.last != Point(p)) o.shape.contours.back().addEdge(msdfgen::EdgeHolder(o.last, Point(p)));
    o.last = Point(p); return 0;
}
int Conic(FT_Vector* c, FT_Vector* p, void* user) {
    auto& o = *static_cast<Outline*>(user);
    o.shape.contours.back().addEdge(msdfgen::EdgeHolder(o.last, Point(c), Point(p)));
    o.last = Point(p); return 0;
}
int Cubic(FT_Vector* a, FT_Vector* b, FT_Vector* p, void* user) {
    auto& o = *static_cast<Outline*>(user);
    o.shape.contours.back().addEdge(msdfgen::EdgeHolder(o.last, Point(a), Point(b), Point(p)));
    o.last = Point(p); return 0;
}
}
bool GenerateGlyph(FT_Face face, uint32_t pixelSize, uint32_t code,
    uint32_t baseline, uint32_t padding, Glyph& glyph) {
    glyph = Glyph{};
    if (!face || !FT_IS_SCALABLE(face) || !pixelSize || pixelSize > 32 ||
        (padding != 0 && padding != 2 && padding != 4)) return false;
    const FT_UInt index = FT_Get_Char_Index(face, code);
    if (!index || FT_Set_Pixel_Sizes(face, pixelSize, 0) ||
        FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
        face->glyph->format != ft_glyph_format_outline) return false;
    Outline outline;
    const FT_Outline_Funcs funcs{Move, Line, Conic, Cubic, 0, 0};
    if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &outline)) return false;
    outline.Close();
    if (FT_Render_Glyph(face->glyph, ft_render_mode_normal)) return false;
    const auto slot = face->glyph;
    glyph.width = slot->bitmap.width;
    glyph.height = std::min<uint32_t>(slot->bitmap.rows, pixelSize);
    glyph.cellHeight = pixelSize + padding;
    glyph.advance = (slot->metrics.horiAdvance / 64) + 1.0f;
    glyph.bearing = slot->metrics.horiBearingX / 64.0f;
    const bool empty = !glyph.width || !glyph.height || !outline.shape.edgeCount();
    if (empty) {
        glyph.width = (pixelSize + 3) >> 2;
        glyph.height = pixelSize;
    } else {
        // Same cell placement/clipping as IGxuFontGlyphRenderGlyph.
        const int32_t top = slot->bitmap_top;
        int32_t start = std::max(0, int32_t(baseline) - top);
        glyph.yOffset = top > int32_t(baseline) ? top - int32_t(baseline) : 0;
        const int32_t available = int32_t(pixelSize - glyph.height);
        if (available >= start) glyph.yStart = start;
        else { glyph.yOffset = int32_t(pixelSize) - start - int32_t(glyph.height); glyph.yStart = available; }
    }
    glyph.cellWidth = glyph.width + padding;
    if (glyph.AtlasWidth() > AtlasSize || glyph.AtlasHeight() > AtlasSize) return false;
    glyph.rgba.assign(glyph.AtlasWidth() * glyph.AtlasHeight() * 4, 0);
    if (empty) return true;
    auto& shape = outline.shape;
    shape.normalize();
    if (!shape.validate()) return false;
    shape.orientContours();
    msdfgen::edgeColoringInkTrap(shape, 3.0, 0);
    msdfgen::Bitmap<float, 3> bitmap(glyph.AtlasWidth(), glyph.AtlasHeight());
    // Include Classic outline padding in the placement, without changing advance
    // or bearing. Bitmap rows are top-down; msdfgen's coordinates are bottom-up.
    const double top = slot->bitmap_top + glyph.yStart + padding / 2.0 + Border;
    const msdfgen::Projection projection(msdfgen::Vector2(1),
        msdfgen::Vector2(Border + padding / 2.0 - slot->bitmap_left, glyph.AtlasHeight() - top));
    msdfgen::generateMSDF(bitmap, shape, projection, msdfgen::Range(PixelRange));
    for (uint32_t y = 0; y < glyph.AtlasHeight(); ++y)
        for (uint32_t x = 0; x < glyph.AtlasWidth(); ++x) {
            const float* p = bitmap(x, glyph.AtlasHeight() - 1 - y);
            auto* out = &glyph.rgba[4 * (y * glyph.AtlasWidth() + x)];
            for (unsigned c = 0; c < 3; ++c)
                out[c] = uint8_t(std::clamp(p[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            out[3] = 255;
        }
    return true;
}
bool PasteGlyph(const void* rgba, uint32_t bytes, uint32_t width, uint32_t height,
    uint32_t x, uint32_t y, uint8_t* atlas) {
    if (!rgba || !atlas || !width || !height || width > AtlasSize || height > AtlasSize ||
        x > AtlasSize - width || y > AtlasSize - height || bytes < width * height * 4) return false;
    const auto* source = static_cast<const uint8_t*>(rgba);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(atlas + 4 * ((y + row) * AtlasSize + x), source + row * width * 4, width * 4);
    return true;
}
}
