#include "Engine/Source/Gxu/GxuFontMsdf.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static uint8_t Median(const uint8_t* p) {
    return std::max(std::min(p[0], p[1]), std::min(std::max(p[0], p[1]), p[2]));
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "pass an installed openly licensed outline font");
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        Require(!FT_Init_FreeType(&library), "FT_Init_FreeType");
        Require(!FT_New_Face(library, argv[1], 0, &face), "FT_New_Face");
        unsigned generated = 0;
        for (uint32_t size : {2u, 6u, 9u, 12u, 24u, 32u}) {
            const uint32_t baseline = uint32_t(double(size * face->ascender) /
                (face->ascender + std::abs(face->descender)) + 0.5);
            for (uint32_t padding : {0u, 2u, 4u}) {
                for (uint32_t code : {uint32_t('A'), uint32_t('g'), uint32_t('j'), uint32_t(' '), 0xe9u, 0x3a9u}) {
                    GxuMsdf::Glyph glyph, repeat;
                    Require(GxuMsdf::GenerateGlyph(face, size, code, baseline, padding, glyph), "generate ASCII/Unicode/space");
                    Require(GxuMsdf::GenerateGlyph(face, size, code, baseline, padding, repeat), "repeat generation");
                    Require(glyph.rgba == repeat.rgba, "deterministic edge colouring and pixels");
                    Require(glyph.AtlasWidth() <= 256 && glyph.AtlasHeight() <= 256, "atlas dimensions bounded");
                    Require(glyph.rgba.size() == glyph.AtlasWidth() * glyph.AtlasHeight() * 4, "RGBA pitch/size");
                    Require(glyph.cellHeight == size + padding, "Classic cell height preserved");
                    Require(glyph.advance == (face->glyph->metrics.horiAdvance / 64) + 1.0f, "FreeType advance preserved");
                    Require(glyph.bearing == face->glyph->metrics.horiBearingX / 64.0f, "FreeType bearing preserved");
                    if (code == ' ') {
                        Require(std::all_of(glyph.rgba.begin(), glyph.rgba.end(), [](uint8_t c) { return c == 0; }), "whitespace atlas is empty, including alpha");
                        Require(glyph.advance > 0, "whitespace advances without a visible glyph");
                    } else {
                        unsigned interior = 0, exterior = 0, antialiased = 0;
                        for (size_t i = 0; i < glyph.rgba.size(); i += 4) {
                            const auto distance = Median(&glyph.rgba[i]);
                            interior += distance > 127;
                            exterior += distance < 32;
                            antialiased += (distance / 255.0f - 0.5f) * GxuMsdf::PixelRange + 0.5f > 0;
                        }
                        Require(exterior > 0, "signed exterior distance");
                        // Subpixel-width strokes can have coverage without a
                        // texel centre strictly inside the outline (e.g. 6px j).
                        if (size >= 6) Require(antialiased > 0, "small-text antialiased stroke survives");
                        if (size >= 12) Require(interior > 0, "signed filled interior distance");
                    }
                    // The production latch copies exactly these RGBA bytes, at the
                    // last legal atlas origin; guards detect OOB writes.
                    std::vector<uint8_t> guarded(256 * 256 * 4 + 32, 0x5a);
                    auto* atlas = guarded.data() + 16;
                    const uint32_t x = 256 - glyph.AtlasWidth(), y = 256 - glyph.AtlasHeight();
                    Require(GxuMsdf::PasteGlyph(glyph.rgba.data(), glyph.rgba.size(), glyph.AtlasWidth(), glyph.AtlasHeight(), x, y, atlas), "safe final-cell upload");
                    for (uint32_t row = 0; row < glyph.AtlasHeight(); ++row)
                        Require(!std::memcmp(atlas + 4 * ((y + row) * 256 + x), glyph.rgba.data() + row * glyph.AtlasWidth() * 4, glyph.AtlasWidth() * 4), "atlas rows preserve RGB channels/alpha");
                    Require(!GxuMsdf::PasteGlyph(glyph.rgba.data(), glyph.rgba.size(), glyph.AtlasWidth(), glyph.AtlasHeight(), x + 1, y, atlas), "reject horizontal overflow");
                    Require(!GxuMsdf::PasteGlyph(glyph.rgba.data(), glyph.rgba.size(), glyph.AtlasWidth(), glyph.AtlasHeight(), x, y + 1, atlas), "reject vertical overflow");
                    Require(!GxuMsdf::PasteGlyph(glyph.rgba.data(), glyph.rgba.size() - 1, glyph.AtlasWidth(), glyph.AtlasHeight(), x, y, atlas), "reject truncated payload");
                    Require(std::all_of(guarded.begin(), guarded.begin() + 16, [](uint8_t c) { return c == 0x5a; }) &&
                        std::all_of(guarded.end() - 16, guarded.end(), [](uint8_t c) { return c == 0x5a; }), "atlas guards intact");
                    ++generated;
                }
            }
        }
        GxuMsdf::Glyph invalid, small, large;
        Require(!GxuMsdf::GenerateGlyph(nullptr, 12, 'A', 9, 0, invalid), "reject null face");
        Require(!GxuMsdf::GenerateGlyph(face, 0, 'A', 9, 0, invalid), "reject zero size");
        Require(!GxuMsdf::GenerateGlyph(face, 256, 'A', 9, 0, invalid), "reject oversized glyph");
        Require(!GxuMsdf::GenerateGlyph(face, 12, 'A', 9, 256, invalid), "reject oversized padding");
        Require(!GxuMsdf::GenerateGlyph(face, 12, 0x10ffff, 9, 0, invalid), "missing Unicode glyph is not .notdef");
        Require(GxuMsdf::GenerateGlyph(face, 9, 'A', 7, 0, small) &&
            GxuMsdf::GenerateGlyph(face, 24, 'A', 19, 0, large), "regenerate on resize");
        Require(small.AtlasHeight() != large.AtlasHeight() && small.rgba != large.rgba, "resized atlas data differs");
        FT_Done_Face(face); FT_Done_FreeType(library);
        std::cout << "PASS: " << generated << " deterministic glyph/outline/size cases; Unicode, whitespace, missing glyph, RGBA atlas bounds, resize\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
