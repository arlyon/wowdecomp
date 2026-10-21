// Non-interactive native WSI regression. Uses only the installed test font;
// no game assets, account state or screenshots. Supports separate validation runs.
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/GxuFont.h"
#include "Engine/Source/Gxu/GxuFontFont.h"
#include "Engine/Source/Gxu/GxuFontMiscClasses.h"
#include "Engine/Source/Gxu/GxuFontFace.h"
#include "Engine/Source/Gxu/IGxuFontGlyph.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Services/Texture.h"
#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static unsigned ReadCoverage(CGxDeviceVulkan& device) {
    CiRect rect; rect.minX = rect.minY = 0; rect.maxX = 320; rect.maxY = 240;
    TSGrowableArray<CImVector> pixels;
    device.DeviceReadPixels(rect, pixels);
    Require(pixels.Count() == 320 * 240, "native font framebuffer readback");
    unsigned count = 0;
    for (uint32_t i = 0; i < pixels.Count(); ++i) {
        const auto& p = pixels[i];
        count += std::abs(int(p.r) - 64) > 2 || std::abs(int(p.g) - 48) > 2 || std::abs(int(p.b) - 32) > 2;
    }
    return count;
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "pass the installed test font");
        Require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        auto* window = SDL_CreateWindow("MSDF non-interactive test", 0, 0, 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        Require(window, SDL_GetError());
        CGxDeviceVulkan device; g_theGxDevicePtr = &device;
        CGxFormat format; format.m_window = 1; format.m_maximize = 0; format.m_size = {320, 240};
        Require(device.DeviceCreate(window, format), "native font device creation");
        Require(device.NativeMsdfFontsSupported(), "native MSDF shader support");
        const auto textureCount = [&]() {
            unsigned count = 0;
            for (auto* texture = device.m_textures.Head(); texture; texture = device.m_textures.Next(texture)) ++count;
            return count;
        };
        const auto initialTextures = textureCount();
        device.m_cursorVisible = 0;
        device.RsSet(GxRs_ColorWrite, 15);
        C44Matrix identity;
        device.XformPushAndSet(GxXform_World, identity);
        device.XformSetView(identity);
        SFile::Initialize(); SFile::EnableDirectAccess(1);
        GxuFontInitialize();
        Require(GxuFontMsdfEnabled() && GxuFontMsdfSupported(), "new fonts default on for Vulkan");
        unsigned previousCoverage = 0;
        for (uint32_t fontFlags : {0u, 1u, 8u}) {
            CGxFont* face = nullptr;
            const float height = 18.0f / 240.0f;
            Require(GxuFontCreateFont(argv[1], height, face, fontFlags), "create installed font through production Gxu");
            Require(face->m_isMsdf, "scalable native face selects MSDF");
            auto* batch = GxuFontCreateBatch(false, false);
            CGxString* string = nullptr;
            Require(GxuFontCreateString(face, "Agj \xc3\xa9 \xce\xa9", height, C3Vector(.1f, .6f, 0),
                .8f, .3f, 0, string, GxVJ_Top, GxHJ_Left, 0, CImVector(255,255,255,255), 0, 1), "production UTF-8 string layout");
            GxuFontAddToBatch(batch, string);
            device.SceneClear(3, CImVector(32,48,64,255));
            GxuFontRenderBatch(batch);
            const unsigned coverage = ReadCoverage(device);
            Require(coverage > 30 && coverage < 8000, "generated atlas produces bounded visible native text");
            Require(coverage > previousCoverage, "outline modes expand visible coverage on coloured background");
            previousCoverage = coverage;
            Require(face->m_activeCharacters.Ptr('A', HASHKEY_NONE()) != nullptr, "production glyph cache populated");
            device.ScenePresent();
            for (uint32_t code : {uint32_t('A'), uint32_t('g'), uint32_t('j'), 0xe9u, 0x1dfu}) {
                GLYPHBITMAPDATA msdf, classic;
                Require(face->GetGlyphData(&msdf, code), "MSDF layout metrics");
                const uint32_t padding = fontFlags == 8 ? 4 : (fontFlags == 1 ? 2 : 0);
                Require(IGxuFontGlyphRenderGlyph(FontFaceGetFace(face->m_fontFaceHandle), face->m_pixelSize,
                    code, face->m_baseline, &classic, 0, padding), "Classic glyph generation regression");
                Require(msdf.m_glyphAdvance == classic.m_glyphAdvance && msdf.m_glyphBearing == classic.m_glyphBearing &&
                    msdf.m_glyphCellWidth == classic.m_glyphCellWidth && msdf.m_yOffset == classic.m_yOffset &&
                    msdf.m_yStart == classic.m_yStart, "MSDF preserves Classic advance/bearing/cell placement");
            }
            const float msdfStep = face->ComputeStep('A', 'g');
            GxuFontSetMsdfEnabled(false);
            Require(!face->m_isMsdf && !face->m_activeCharacters.m_fulllist.Head(), "Classic mode invalidates glyphs");
            Require(face->NewCodeDesc('A') && face->NewCodeDesc('g'), "Classic cached layout glyphs");
            Require(face->ComputeStep('A', 'g') == msdfStep, "Classic/MSDF kerning and advance agree");
            GxuFontSetMsdfEnabled(true);
            Require(face->m_isMsdf && !face->m_activeCharacters.m_fulllist.Head(), "MSDF mode invalidates Classic cache");
            device.SceneClear(3, CImVector(32,48,64,255));
            GxuFontRenderBatch(batch);
            Require(ReadCoverage(device) == coverage, "mode round-trip rebuilds identical native string geometry");
            device.ScenePresent();
            GxuFontAddShadow(string, CImVector(0,0,0,255), C2Vector(3.0f/320, -3.0f/240));
            device.SceneClear(3, CImVector(32,48,64,255));
            GxuFontRenderBatch(batch);
            Require(ReadCoverage(device) > coverage, "production shadow geometry uses native font shader");
            device.ScenePresent();
            GxuFontRemoveShadow(string);
            if (device.SceneTemporalSupported()) {
                for (int frame = 0; frame < 2; ++frame) {
                    device.SceneClear(3, CImVector(32,48,64,255));
                    Require(device.SceneBeginWorld(true, 17 + fontFlags), "begin temporal font scope");
                    GxuFontRenderBatch(batch);
                    device.SceneEndWorld();
                    Require(ReadCoverage(device) == coverage, "reactive MSDF text changed temporal/native coverage");
                    device.ScenePresent();
                }
            }
            if (device.SceneHdrSupported()) {
                Require(device.SceneSetHdr(true, 0), "enable float scene for font boundary regression");
                for (bool temporal : {false, true}) {
                    device.SceneClear(3, CImVector(32,48,64,255));
                    Require(device.SceneBeginWorld(temporal, 31 + fontFlags), "begin float font scope");
                    GxuFontRenderBatch(batch);
                    device.SceneEndWorld();
                    const unsigned sceneCoverage = ReadCoverage(device);
                    Require(sceneCoverage > 30 && sceneCoverage < 8000, "float/temporal authored font variant lost bounded text");
                    device.ScenePresent();
                }
                device.SceneClear(3, CImVector(32,48,64,255));
                GxuFontRenderBatch(batch);
                Require(ReadCoverage(device) == coverage, "requested float transport changed native UI text outside world scope");
                device.ScenePresent();
                Require(device.SceneSetHdr(false, 0), "restore Classic scene transport");
            }
            if (fontFlags == 8) {
                unsigned cached = 0;
                FT_Face ft = FontFaceGetFace(face->m_fontFaceHandle);
                for (uint32_t code = 33; code < 0x600; ++code) {
                    if (!FT_Get_Char_Index(ft, code)) continue;
                    if (!face->NewCodeDesc(code)) {
                        GLYPHBITMAPDATA probe;
                        const bool generated = face->GetGlyphData(&probe, code);
                        std::cerr << "cache glyph U+" << std::hex << code << std::dec << " generated=" << generated << " atlasWidth=" << (generated ? probe.AtlasWidth() : 0) << '\n';
                        Require(false, "cache allocation/eviction preserves Unicode glyphs");
                    }
                    ++cached;
                }
                Require(cached > 800 && face->m_textureCache[7].m_texture, "stress all eight atlas pages and eviction");
                for (auto& page : face->m_textureCache)
                    for (uint32_t row = 0; row < page.m_textureRows.m_count; ++row) {
                        auto& list = page.m_textureRows[row].m_glyphlist;
                        for (auto* desc = list.Head(); desc; desc = list.Next(desc))
                            Require(desc->m_glyphEndPixel < 256 &&
                                (desc->m_rowNumber + 1) * face->AtlasCellHeight() <= 256, "production atlas allocation remains in bounds");
                    }
                device.SceneClear(3, CImVector(32,48,64,255));
                GxuFontRenderBatch(batch);
                Require(ReadCoverage(device) == coverage, "evicted string geometry rebuilds identically");
                device.ScenePresent();
                const uint32_t oldSize = face->m_pixelSize;
                CGxFormat resized = format; resized.m_size = {320,300};
                Require(device.DeviceSetFormat(resized), "native resize");
                device.DeviceWM(GxWM_Size, 0, 0);
                GxuFontWindowSizeChanged();
                Require(face->m_pixelSize != oldSize && !face->m_activeCharacters.m_fulllist.Head(), "screen resize invalidates glyph cache and metrics");
                Require(device.DeviceSetFormat(format), "restore native test size");
                device.DeviceWM(GxWM_Size, 0, 0);
                GxuFontWindowSizeChanged();
                device.SceneClear(3, CImVector(32,48,64,255));
                GxuFontRenderBatch(batch);
                Require(ReadCoverage(device) == coverage, "resize round-trip rebuilds identical native text");
                device.ScenePresent();
                std::cout << "PASS: production atlas eviction " << cached << " Unicode glyphs, shadow and screen resize\n";
            }
            GxuFontDestroyString(string);
            Require(GxuFontCreateString(face, " ", height, C3Vector(.1f,.6f,0), .8f,.3f,0,
                string,GxVJ_Top,GxHJ_Left,0,CImVector(255,255,255,255),0,1), "whitespace layout");
            GxuFontAddToBatch(batch,string);
            device.SceneClear(3, CImVector(32,48,64,255));
            GxuFontRenderBatch(batch);
            Require(ReadCoverage(device) == 0, "whitespace never becomes outline rectangle");
            device.ScenePresent();
            GxuFontDestroyString(string); GxuFontDestroyBatch(batch); GxuFontDestroyFont(face);
            std::cout << "PASS: production Gxu native glyph/cache/layout/draw outline=" << fontFlags << " coveredPixels=" << coverage << '\n';
        }
        CGxFont* mono = nullptr;
        Require(GxuFontCreateFont(argv[1], 18.0f/240.0f, mono, 2) && !mono->m_isMsdf, "monochrome font retains Classic");
        CGxString* classic = nullptr;
        auto* classicBatch = GxuFontCreateBatch(false, false);
        Require(GxuFontCreateString(mono, "Classic", 18.0f/240.0f, C3Vector(.1f,.6f,0), .8f,.3f,0,
            classic,GxVJ_Top,GxHJ_Left,0,CImVector(255,255,255,255),0,1), "Classic fallback layout");
        GxuFontAddToBatch(classicBatch, classic);
        device.SceneClear(3, CImVector(32,48,64,255));
        GxuFontRenderBatch(classicBatch);
        Require(ReadCoverage(device) > 30, "monochrome Classic fallback still draws through native backend");
        device.ScenePresent();
        GxuFontDestroyString(classic); GxuFontDestroyBatch(classicBatch); GxuFontDestroyFont(mono);
        std::cout << "PASS: production monochrome Classic fallback draw\n";
        GxuFontShutdown();
        TextureGxCacheFlush();
        Require(textureCount() == initialTextures, "font/cache teardown retained texture objects");
        device.DeviceDestroy(); g_theGxDevicePtr = nullptr;
        SDL_DestroyWindow(window); SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
