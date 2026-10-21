// Standalone bundled-FreeType regression: no Gxu, renderer, atlas or msdfgen.
#include <freetype/freetype.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        Require(argc >= 2, "pass an installed openly licensed font");
        FT_Library library = nullptr;
        Require(!FT_Init_FreeType(&library), "initialise default FreeType allocator");
        for (const auto flags : {FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING,
            FT_LOAD_LINEAR_DESIGN | FT_LOAD_PEDANTIC | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING}) {
            FT_Face face = nullptr;
            Require(!FT_New_Face(library, argv[1], 0, &face), "load installed test font");
            Require(!FT_Set_Pixel_Sizes(face, 18, 0), "set pixel size");
            for (uint32_t code = 33; code < 0x600; ++code) {
                auto index = FT_Get_Char_Index(face, code);
                if (!index) continue;
                if (argc > 2) std::cerr << "loading U+" << std::hex << code << " index=" << index << std::dec << " flags=" << flags << '\n';
                Require(!FT_Load_Glyph(face, index, flags), "load Unicode glyph");
                Require(!FT_Render_Glyph(face->glyph, ft_render_mode_normal), "render Unicode glyph");
            }
            FT_Done_Face(face);
        }
        FT_Done_FreeType(library);
        std::cout << "PASS: bundled FreeType composite Unicode loading/rendering with MSDF and Classic load flags, default allocator\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
