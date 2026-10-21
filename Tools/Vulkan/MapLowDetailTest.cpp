// Source-owned archive fixture: production WDL loader, selector, horizon list and draw.
// No installed game data, world/account state or replacement terrain mesh.
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/FarAdtRender.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/SortTable.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "WoW/Source/WorldClient/HorizonState.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include <SDL.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <atomic>
#include <array>

// This fixture isolates the WDL/coarse fallback. Prepared ADT production draws
// are exercised by TerrainDistanceWorldGpu and TerrainDistanceNorthrendGpu.
bool FarAdtRenderTile(CMapAreaLow*) { return false; }

namespace {
std::atomic<unsigned> textureBodyReads{0}, synchronousBodyReads{0}, hardWaits{0};
std::thread::id mainThread;
std::atomic<bool> holdBody{false}, bodyHeld{false}, corruptBody{false};
void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
CVar setting;
CFrustum frustum;
unsigned rejectVolume = 0, rejectBuffer = 0;
constexpr unsigned RidgeId = 49 * 64 + 32;
DayNight::DNInfo info;
std::atomic<unsigned> textureHeaderReads{0};
void Word(std::vector<char> &out, uint32_t word) {
    const size_t n = out.size();
    out.resize(n + 4);
    std::memcpy(out.data() + n, &word, 4);
}
void Save(const std::filesystem::path &path, const std::vector<char> &data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), data.size());
    Check(bool(file), "fixture write failed");
}
std::vector<char> Wdl(bool malformed = false) {
    std::vector<char> out;
    Word(out, 0x4d564552);
    Word(out, 4);
    Word(out, 18);
    Word(out, 0x4d414f46);
    Word(out, 4096 * 4);
    const size_t table = out.size();
    out.resize(table + 4096 * 4);
    for (unsigned tile = 0; tile < 4096; ++tile) {
        const unsigned y = tile / 64, x = tile % 64;
        if (tile != 0 && tile != 4095 && !(y >= 33 && y <= 49 && x >= 30 && x <= 33))
            continue;
        const uint32_t offset = out.size();
        std::memcpy(out.data() + table + (y * 64 + x) * 4, &offset, 4);
        Word(out, 0x4d415245);
        Word(out, 545 * 2);
        for (unsigned v = 0; v < 545; ++v) {
            const auto uv = FarTerrain::UV(v);
            const int16_t height =
                int16_t(600 + 200 * std::sin(uv.x * 3.14159265f) + 120 * std::sin(uv.y * 3.14159265f));
            const size_t n = out.size();
            out.resize(n + 2);
            std::memcpy(out.data() + n, &height, 2);
        }
        Word(out, 0x4d41484f);
        Word(out, 32);
        for (unsigned row = 0; row < 16; ++row) {
            const uint16_t holes = (x == 30 ? 0xffff : 0);
            const size_t n = out.size();
            out.resize(n + 2);
            std::memcpy(out.data() + n, &holes, 2);
        }
    }
    // Both map corners and a malformed nonzero offset must remain bounded.
    uint32_t invalid = uint32_t(out.size() + 100);
    std::memcpy(out.data() + table + 4094 * 4, &invalid, 4);
    if (malformed)
        out.resize(23);
    return out;
}
std::vector<char> Blp() {
    BLPHeader header{};
    header.m_magic = 0x32504c42;
    header.m_formatVersion = 1;
    header.m_colorEncoding = 3;
    header.m_alphaSize = 8;
    header.m_preferredFormat = 2;
    header.m_hasMips = 1;
    header.m_width = 16;
    header.m_height = 16;
    std::vector<char> out(sizeof(header));
    for (unsigned level = 0, size = 16; size; size >>= 1, ++level) {
        header.m_mipOffsets[level] = out.size();
        header.m_mipSizes[level] = size * size * 4;
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x) {
                // BGRA asymmetric source colours detect UV axis swaps and flat/fog output.
                out.push_back(char(20 + 180 * y / std::max(1u, size - 1)));
                out.push_back(char(35 + 100 * x / std::max(1u, size - 1)));
                out.push_back(char(210 - 120 * y / std::max(1u, size - 1)));
                out.push_back(char(255));
            }
    }
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}
void SetFrustum(float far) {
    const float near = setting.m_floatValue > 0 ? World::s_farClip * .75f : World::s_farClip - 50.f;
    frustum.planes[0].n = C3Vector(0, 1, 0);
    frustum.planes[0].d = 10000;
    frustum.planes[1].n = C3Vector(0, -1, 0);
    frustum.planes[1].d = 10000;
    frustum.planes[2].n = C3Vector(0, 0, 1);
    frustum.planes[2].d = 10000;
    frustum.planes[3].n = C3Vector(0, 0, -1);
    frustum.planes[3].d = 10000;
    frustum.planes[4].n = C3Vector(-1, 0, 0);
    frustum.planes[4].d = 266.f - near;
    frustum.planes[5].n = C3Vector(1, 0, 0);
    frustum.planes[5].d = far - 266.f;
}
void Cull(float distance) {
    setting.m_floatValue = distance;
    SetFrustum(World::GetHorizonDistance());
    CWorldScene::sortTable.m_horizonChunkList.UnlinkAll();
    C3Vector camera(266, 0, 100);
    CMap::s_lowDetail.CullHorizonAreas(&camera);
}
void Draw() {
    auto &list = CWorldScene::sortTable.m_horizonChunkList;
    for (auto *area = list.Head(); area;) {
        auto *next = list.Next(area);
        CMap::RenderLowDetail(area);
        area->m_horizonLink.Unlink();
        area = next;
    }
}
void Cpu(const std::filesystem::path &root) {
    auto &low = CMap::s_lowDetail;
    low.Load("World\\Maps\\Fixture", "Fixture");
    Check(low.GetCensus().wdlRead && low.GetCensus().nonzeroOffsets == 71 && low.GetCensus().resident == 70 &&
              low.GetCensus().malformed == 1,
          "production WDL census/validation wrong");
    Check(low.m_areas[0] && low.m_areas[4095] && !low.m_areas[4094] && !low.m_areas[1],
          "zero/malformed/map-edge residency wrong");
    Check(low.m_areas[RidgeId] && low.m_areas[RidgeId]->m_heights[0] == 600, "known ridge missing");
    Check(low.m_areas[33 * 64 + 30]->m_indexCount == 0, "holes ignored");
    Cull(0);
    Check(!low.GetCensus().admittedIds[RidgeId], "Classic admitted 9k ridge");
    const auto classic = low.GetCensus();
    Cull(4000);
    Check(!low.GetCensus().admittedIds[RidgeId] && low.GetCensus().admittedIds[39 * 64 + 32] &&
              !low.GetCensus().admittedIds[46 * 64 + 32],
          "4000 production ridge inventory incorrect");
    Cull(10000);
    Check(low.GetCensus().admittedIds[46 * 64 + 32] && low.GetCensus().admittedIds[RidgeId],
          "10000 did not admit real WDL ridge");
    Check(low.GetCensus().detailFar == classic.detailFar, "detail budget changed");
    Check(low.GetCensus().textureRequests <= 8 && low.GetCensus().inFlightBytes <= 8 * FarTerrain::MaxTextureFileBytes,
          "request/body budget exceeded");
    rejectVolume = 1;
    Cull(10000);
    Check(low.GetCensus().volumeRejects > 0 && !low.GetCensus().admitted, "volume rejection census wrong");
    rejectVolume = 0;
    rejectBuffer = 1;
    Cull(10000);
    Check(low.GetCensus().bufferRejects > 0 && !low.GetCensus().admitted, "clip-buffer rejection census wrong");
    rejectBuffer = 0;
    const auto *area = low.m_areas[RidgeId];
    CGxVertexPCT v{};
    FarTerrain::Vertex(v, area->m_topLeft, area->m_heights, 16, C3Vector(0, 0, 1), CImVector(30, 20, 10, 255),
                       CImVector(0, 0, 0, 255));
    Check(v.tc.x == 1 && v.tc.y == 0 && v.p.y < 0 && v.p.x == area->m_topLeft.x && v.c.r == 10 && v.c.b == 30,
          "world/UV/colour orientation incorrect");
    for (unsigned i = 0; i < 545; ++i) {
        const auto uv = FarTerrain::UV(i);
        Check(uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1, "UV out of tile");
    }
    C3Vector edge(40000, 40000, 0);
    low.CullHorizonAreas(&edge);
    Check(low.GetCensus().candidates == 0, "positive map edge OOB");
    edge = C3Vector(-40000, -40000, 0);
    low.CullHorizonAreas(&edge);
    Check(low.GetCensus().candidates == 0, "negative map edge OOB");
    low.Load("World\\Maps\\Fixture", "Bad");
    Check(!low.GetCensus().resident && low.GetCensus().malformed, "malformed table accepted");
    low.Load("World\\Maps\\Fixture", "Fixture");
}
void Gpu(CGxDeviceVulkan &device) {
    auto &low = CMap::s_lowDetail;
    info.m_fog.m_color = CImVector(170, 180, 190, 255);
    info.m_fog.m_start = 7000;
    info.m_fog.m_end = 10000;
    info.m_light0.m_dir = C3Vector(0, 0, 1);
    info.m_light0.m_ambient = CImVector(255, 255, 255, 255);
    info.m_light0.m_diffuse = CImVector(0, 0, 0, 255);
    CMap::s_vbbList.Initialize(GxPoolTarget_Vertex, GxPoolUsage_Static, 16, 545, 24);
    auto *indices =
        device.PoolCreate(GxPoolTarget_Index, GxPoolUsage_Static, 3072 * 2, GxPoolHintBit_Unk3, "WDL indices");
    CMap::lowDetailIndexBuf = device.BufCreate(indices, 2, 3072, 0);
    C44Matrix identity;
    device.XformPushAndSet(GxXform_World, identity);
    // Camera looks along world -X; horizontal world -Y and vertical world +Z.
    C44Matrix view(0, 0, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, -100, 266, 1);
    device.XformSetView(view);
    C44Matrix projection;
    GxuXformCreateProjection_Exact(1.5707964f, 4.f / 3.f, World::s_farClip * .75f, 10000, projection);
    device.XformSetProjection(projection);
    device.RsSet(GxRs_ColorWrite, 15);
    device.RsSet(GxRs_DepthFunc, 0);
    device.RsSet(GxRs_DepthTest, 1);
    device.RsSet(GxRs_DepthWrite, 1);
    HorizonState::Bind(device);
    bool ready = false;
    for (unsigned frame = 1; frame < 100; ++frame) {
        CWorld::frameCnt = frame;
        AsyncFileReadPollHandler(nullptr, nullptr);
        device.XformSetViewport(0, 1, 0, 1, 0, 1);
        device.SceneClear(3, CImVector(9, 17, 25, 255));
        Check(device.SceneBeginWorld(true, frame), "world scope failed");
        const auto drawStart = std::chrono::steady_clock::now();
        Cull(10000);
        Draw();
        const auto drawMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - drawStart).count();
        Check(low.GetCensus().submittedIds[RidgeId], "production ridge not submitted");
        Check(low.GetCensus().triangles > 0 && low.GetCensus().streamedMeshes > 0, "24-block fallback absent");
        Check(device.m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0] == 1, "depth caller state leaked");
        Check(device.m_appRenderStates[GxRs_Texture0].m_value.m_data.p == nullptr, "texture caller state leaked");
        // Isolate the actual 9k resident ridge, not an authored quad.
        if (low.GetFarTexture(low.m_areas[RidgeId])) {
            device.SceneClear(3, CImVector(9, 17, 25, 255));
            device.RsSet(GxRs_Lighting, 1);
            device.RsSet(GxRs_FogStart, 123.f);
            device.RsSet(GxRs_FogEnd, 456.f);
            device.RsSet(GxRs_Texture1, low.GetFarTexture(low.m_areas[RidgeId]));
            std::array<CGxStateBom, GxRenderStates_Last> before;
            for (unsigned i = 0; i < GxRenderStates_Last; ++i)
                before[i] = device.m_appRenderStates[i].m_value;
            const auto viewport = device.m_viewport;
            const auto projectionBefore = device.m_projection;
            CMap::RenderLowDetail(low.m_areas[RidgeId]);
            for (unsigned i = 0; i < GxRenderStates_Last; ++i)
                Check(std::memcmp(&before[i], &device.m_appRenderStates[i].m_value, sizeof(CGxStateBom)) == 0,
                      "LoD leaked caller render state");
            Check(std::memcmp(&viewport, &device.m_viewport, sizeof(viewport)) == 0 &&
                      projectionBefore == device.m_projection,
                  "LoD leaked viewport/projection");
            device.RsSet(GxRs_Texture1, static_cast<void *>(nullptr));
            device.RsSet(GxRs_Lighting, 0);
            CiRect rect;
            rect.minX = 148;
            rect.maxX = 172;
            rect.minY = 100;
            rect.maxY = 120;
            TSGrowableArray<CImVector> pixels;
            device.DeviceReadPixels(rect, pixels);
            unsigned surface = 0;
            int minGreen = 255, maxGreen = 0;
            int minRed = 255, maxRed = 0;
            std::array<unsigned, 24> greenSum{}, greenCount{};
            for (unsigned i = 0; i < pixels.Count(); ++i) {
                const auto p = pixels[i];
                if (p.r == 25 && p.b == 9)
                    continue;
                if (std::abs(int(p.r) - 190) <= 5 && std::abs(int(p.b) - 170) <= 5)
                    continue;
                greenSum[i % 24] += p.g;
                ++greenCount[i % 24];
                ++surface;
                minGreen = std::min(minGreen, int(p.g));
                maxGreen = std::max(maxGreen, int(p.g));
                minRed = std::min(minRed, int(p.r));
                maxRed = std::max(maxRed, int(p.r));
            }
            std::cout << "9k ridge readback surfacePixels=" << surface << " green=" << minGreen << ".." << maxGreen
                      << " red=" << minRed << ".." << maxRed << "\n";
            Check(surface > 0 && maxGreen - minGreen > 3, "9k ridge colour/UVs collapsed to flat fog");
            unsigned left = 0, right = 23;
            while (left < 23 && !greenCount[left])
                ++left;
            while (right > 0 && !greenCount[right])
                --right;
            Check(right > left && greenSum[right] / greenCount[right] > greenSum[left] / greenCount[left] + 3,
                  "minimap U/world-Y orientation reversed");
            std::cout << "Warm production census: candidates=" << low.GetCensus().candidates
                      << " admitted=" << low.GetCensus().admitted << " tiles=" << low.GetCensus().submittedTiles
                      << " tris=" << low.GetCensus().triangles << " CPU-cull/draw-us=" << drawMicros << "\n";
            ready = true;
        }
        device.SceneEndWorld();
        device.ScenePresent();
        if (ready)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(ready, "async pending never became ready");
    Check(textureBodyReads > 0 && synchronousBodyReads == 0 && hardWaits == 0,
          "normal async loader reloaded/waited on camera thread");
    Check(low.GetCensus().texturedIds[RidgeId] && low.GetCensus().maxTexturedDistance > 9000,
          "textured census did not identify ridge");
    std::cout << "Async bodies=" << textureBodyReads << " headers=" << textureHeaderReads
              << " mainThreadBodies=" << synchronousBodyReads << " hardWaits=" << hardWaits << "\n";
    Check(low.GetCensus().maxSubmittedDistance > 9000, "draw distance census missing ridge");
    auto *invalidated = low.GetFarTexture(low.m_areas[RidgeId]);
    Check(invalidated, "ridge texture missing before recreation");
    invalidated->m_needsCreation = 1;
    Check(!low.GetFarTexture(low.m_areas[RidgeId]), "native texture loss attempted synchronous Draw reload");
    bool recreated = false;
    for (unsigned i = 0; i < 40 && !recreated; ++i) {
        ++CWorld::frameCnt;
        AsyncFileReadPollHandler(nullptr, nullptr);
        Cull(10000);
        recreated = low.GetFarTexture(low.m_areas[RidgeId]) != nullptr;
        device.ScenePresent();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(recreated && synchronousBodyReads == 0 && hardWaits == 0,
          "texture recreation did not use bounded async path");
    std::cout << "Native texture invalidation recovered asynchronously without body reload/wait on camera thread\n";
    low.Purge();
    ++CWorld::frameCnt;
    Cull(0);
    device.SceneClear(3, CImVector(9, 17, 25, 255));
    CMap::RenderLowDetail(low.m_areas[33 * 64 + 32]);
    CiRect classicRect;
    classicRect.minX = 0;
    classicRect.maxX = 320;
    classicRect.minY = 0;
    classicRect.maxY = 240;
    TSGrowableArray<CImVector> classicPixels;
    device.DeviceReadPixels(classicRect, classicPixels);
    unsigned classicSurface = 0;
    for (unsigned i = 0; i < classicPixels.Count(); ++i) {
        const auto p = classicPixels[i];
        if (p.r == 25 && p.g == 17 && p.b == 9)
            continue;
        Check(p.r == 190 && p.g == 180 && p.b == 170, "Classic changed prior fog-coloured WDL output");
        ++classicSurface;
    }
    Check(classicSurface > 0, "Classic baseline fixture not visible");
    std::cout << "Classic baseline fogPixels=" << classicSurface << "\n";
    Check(!low.GetCensus().submittedIds[RidgeId] && low.GetCensus().textureReady == 0, "Classic entered modern path");
    low.Unload();
    AsyncFileReadPollHandler(nullptr, nullptr);
    Check(low.GetCensus().resident == 0, "unload retained tiles");
    CMap::s_vbbList.Destroy();
    device.BufDestroy(CMap::lowDetailIndexBuf);
    device.PoolDestroy(indices);
    GxXformPop(GxXform_World);
}
void Lifetime() {
    auto &low = CMap::s_lowDetail;
    CWorld::frameCnt = 1000;
    holdBody = true;
    bodyHeld = false;
    low.Load("World\\Maps\\Fixture", "Fixture");
    Cull(10000);
    Check(low.GetCensus().textureRequests == 8, "initial request budget wrong");
    for (unsigned i = 0; i < 100 && !bodyHeld; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(bodyHeld, "normal async body was not held for generation test");
    const auto generation = low.GetCensus().generation;
    low.Load("World\\Maps\\Fixture", "Bad");
    Check(low.GetCensus().generation == generation + 1 && !low.GetCensus().resident, "map generation did not reset");
    holdBody = false;
    for (unsigned i = 0; i < 30; ++i) {
        AsyncFileReadPollHandler(nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Cull(10000);
    Check(!low.GetCensus().cacheEntries && !low.GetCensus().resident, "stale map callback republished terrain");
    low.Load("World\\Maps\\Fixture", "Fixture");
    Cull(10000);
    Check(!low.GetCensus().textureRequests, "map reload bypassed per-frame budget");
    corruptBody = true;
    ++CWorld::frameCnt;
    Cull(10000);
    for (unsigned i = 0; i < 100; ++i) {
        AsyncFileReadPollHandler(nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Cull(10000);
    for (auto *area : low.m_areas)
        if (area && area->m_indexCount)
            low.GetFarTexture(area);
    Check(!low.GetCensus().texturePending && low.GetCensus().textureMissing > 0,
          "invalid async body remained pending or became white terrain");
    corruptBody = false;
    const unsigned headers = textureHeaderReads;
    for (unsigned i = 0; i < 12; ++i) {
        low.Load("World\\Maps\\Fixture", "Fixture");
        Cull(10000);
        Check(low.GetCensus().textureRequests == 0 && low.GetCensus().cacheEntries <= FarTerrain::TextureCapacity &&
                  low.GetCensus().inFlightBytes <= FarTerrain::InFlightLimit * FarTerrain::MaxTextureFileBytes,
              "reload/cache budget escaped");
    }
    Check(textureHeaderReads == headers, "same-frame traversal created an async reload storm");
    low.Unload();
    Check(!low.GetCensus().resident && !low.GetCensus().cacheEntries, "unload retained terrain cache");
    Check(synchronousBodyReads == 0 && hardWaits == 0, "lifetime path hard-waited or reloaded bodies");
    std::cout << "Map-generation cancellation, invalid-body failure, 12 reloads/frame and bounded traversal passed\n";
}
} // namespace

extern "C" int RealRead(SFileImpl *, void *, uint32_t, uint32_t *, SOVERLAPPED *,
                        uint32_t) asm("__real__Z13SFileReadFileP9SFileImplPvjPjP11SOVERLAPPEDj");
extern "C" int ObserveRead(SFileImpl *file, void *data, uint32_t count, uint32_t *read, SOVERLAPPED *overlap,
                           uint32_t flags) asm("__wrap__Z13SFileReadFileP9SFileImplPvjPjP11SOVERLAPPEDj");
extern "C" int ObserveRead(SFileImpl *file, void *data, uint32_t count, uint32_t *read, SOVERLAPPED *overlap,
                           uint32_t flags) {
    const bool texture = file->m_name && FarTerrain::Key(file->m_name).find(".blp") != std::string::npos;
    const bool body = texture && count > sizeof(BLPHeader);
    if (texture) {
        if (body) {
            ++textureBodyReads;
            if (std::this_thread::get_id() == mainThread)
                ++synchronousBodyReads;
            while (holdBody) {
                bodyHeld = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else
            ++textureHeaderReads;
    }
    const int result = RealRead(file, data, count, read, overlap, flags);
    if (body && corruptBody)
        std::memset(data, 0, sizeof(BLPHeader));
    return result;
}
extern "C" void RealWait(CAsyncObject *) asm("__real__Z17AsyncFileReadWaitP12CAsyncObject");
extern "C" void ObserveWait(CAsyncObject *object) asm("__wrap__Z17AsyncFileReadWaitP12CAsyncObject");
extern "C" void ObserveWait(CAsyncObject *object) {
    ++hardWaits;
    RealWait(object);
}

// Only world/object services absent from this source-owned scene are substituted.
CMapLowDetail CMap::s_lowDetail;
FVBBList CMap::s_vbbList;
CGxBuf *CMap::lowDetailIndexBuf = nullptr;
int32_t CMap::s_isStreamingMode = 1;
int32_t CWorld::frameCnt = 1;
float World::s_farClip = 2375.f / 3;
float World::s_horizonFarClipScale = 3;
CVar *CWorldParam::cvar_landscapeDistance = &setting;
float World::GetHorizonDistance() {
    return setting.m_floatValue > 0 ? setting.m_floatValue : s_farClip * s_horizonFarClipScale;
}
CMapAreaLow *CMap::AllocAreaLow() {
    return new CMapAreaLow;
}
void CMap::FreeAreaLow(CMapAreaLow *area) {
    if (area->m_block)
        s_vbbList.Release(area->m_block);
    delete area;
}
CMapObjDef *CMap::CreateMapObjDef(char *, SMMapObjDef *, C3Vector *, bool) {
    throw std::runtime_error("unexpected object streaming");
}
void CMap::PurgeMapObjDef(CMapObjDef *) {
    throw std::runtime_error("unexpected object purge");
}
void CMapObj::WaitLoad() {
    throw std::runtime_error("unexpected object wait");
}
void CMapObj::ReadGroup(uint32_t) {
    throw std::runtime_error("unexpected group stream");
}
void CMapObj::WaitLoadGroup(uint32_t) {
    throw std::runtime_error("unexpected group wait");
}
int32_t CMapObj::IsGroupLoaded(int32_t) {
    return 0;
}
CVar::CVar() {}
void ConsoleCommandExecute(const char *, int32_t) {}
void ConsolePostClose() {}
void ConsolePrintf(const char *, ...) {}
void ConsoleWrite(const char *, COLOR_T) {}
void ConsoleWriteA(const char *, COLOR_T, ...) {}
CMapBaseObj::~CMapBaseObj() {}
void CMapBaseObj::SelectLights(CM2Lighting *) {}
void CMapBaseObj::SelectUnderwater(CM2Lighting *) {}
CMapObjDef::~CMapObjDef() {}
CMapObjDefGroup::~CMapObjDefGroup() {}
void CMapObjDefGroup::SelectLights(CM2Lighting *) {}
DayNight::CurrentLight::CurrentLight() {}
CSortEntry::CSortEntry() {}
CSortTable::CSortTable() {
    m_horizonChunkList.SetLinkOffset(offsetof(CMapAreaLow, m_horizonLink));
}
CSortTable CWorldScene::sortTable;
int32_t CWorldScene::FrustumCull(CAaBox *box) {
    return frustum.Cull(*box) == 0;
}
void CWorldScene::AddHorizonMapObjDef(CMapObjDef *) {
    throw std::runtime_error("unexpected horizon object");
}
uint32_t CWorldOcclusion::QueryVolumes(CAaSphere *) {
    return rejectVolume;
}
uint32_t CWorldOcclusion::QueryBuffer(CAaBox *, uint8_t) {
    return rejectBuffer;
}
DayNight::DNInfo::DNInfo() {}
DayNight::DNInfo *DayNight::GetInfo() {
    return &info;
}

int main(int argc, char **) {
    mainThread = std::this_thread::get_id();
    char temp[] = "/tmp/wow-wdl-fixture-XXXXXX";
    try {
        Check(mkdtemp(temp) != nullptr, "temp fixture failed");
        const std::filesystem::path root(temp);
        Save(root / "World/Maps/Fixture/Fixture.wdl", Wdl());
        Save(root / "World/Maps/Fixture/Bad.wdl", Wdl(true));
        Save(root / "Textures/Minimap/fixture.blp", Blp());
        std::string translation = "dir: Fixture\r\n";
        for (unsigned y = 33; y <= 49; ++y)
            for (unsigned x = 30; x <= 33; ++x) {
                char line[128];
                std::snprintf(line, sizeof(line), "Fixture\\map%u_%02u.blp\tfixture.blp\r\n", x, y);
                translation += line;
            }
        Save(root / "Textures/Minimap/md5translate.trs", std::vector<char>(translation.begin(), translation.end()));
        SFile::Initialize();
        SFile::EnableDirectAccess(1);
        SFile::SetBasePath(temp);
        Check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        CGxDeviceVulkan device;
        g_theGxDevicePtr = &device;
        CGxFormat format;
        format.m_window = 1;
        format.m_maximize = 0;
        format.m_size = {320, 240};
        Check(device.DeviceCreate(static_cast<GxWindowProc>(nullptr), format) != 0, "device create failed");
        AsyncFileReadInitialize(1, 20);
        TextureInitialize();
        Cpu(root);
        if (argc == 1) {
            Gpu(device);
            Lifetime();
        }
        CMap::s_lowDetail.Unload();
        AsyncFileReadDestroy();
        TextureDestroy();
        device.DeviceDestroy();
        SDL_Quit();
        SFile::SetBasePath("");
        std::filesystem::remove_all(root);
        std::cout << "Production WDL Load/Cull/Add/Draw: ridge ID " << RidgeId
                  << ", async minimap colour, holes, budgets and Classic isolation passed\n";
    } catch (const std::exception &error) {
        holdBody = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
