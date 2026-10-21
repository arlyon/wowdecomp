#include "MapLowDetail.h"
#include "FarAdtRender.h"
#include "WoW/Source/WorldClient/SortTable.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "WoW/Source/WorldClient/FVBBList.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "Engine/Source/Gx/CGxVertexPC.h"

// VERIFY: 82a2066d auto
// OFFSET: 0x00792F60
void CWorldScene::AddHorizonChunk(CMapAreaLow* area) {
    CWorldScene::sortTable.m_horizonChunkList.LinkToTail(area);
}

namespace {
void DestroyFarTexture(CTexture *texture) {
    if (!texture)
        return;
    // These are private LoD textures, not entries in the global reusable texture cache.
    if (texture->m_gxTex) {
        g_theGxDevicePtr->TexDestroy(texture->m_gxTex);
        texture->m_gxTex = nullptr;
    }
    HandleClose(reinterpret_cast<HOBJECT>(texture));
}
void FarTextureFailed(CTexture *texture) {
    AsyncTextureFailureCallback(texture);
    texture->m_asyncObject = nullptr;
    texture->m_status.Add(STATUS_ERROR, "Far terrain texture body read failed");
}
void FarTextureLoaded(CTexture *texture) {
    const auto *object = texture->m_asyncObject;
    BLPHeader header{};
    if (!object->m_buffer || object->m_size < sizeof(header)) {
        FarTextureFailed(texture);
        return;
    }
    std::memcpy(&header, object->m_buffer, sizeof(header));
    if (!FarTerrain::ValidTexture(header, object->m_size)) {
        FarTextureFailed(texture);
        return;
    }
    AsyncTextureSuccessCallback(texture);
}
} // namespace

bool CMapLowDetail::FarTerrainEnabled() const {
    return CWorldParam::cvar_landscapeDistance && CWorldParam::cvar_landscapeDistance->m_floatValue > 0;
}

void CMapLowDetail::LoadTextureNames(const char *mapName) {
    SFileImpl *file = nullptr;
    if (!SFileOpenFile("Textures\\Minimap\\md5translate.trs", &file) || !file)
        return;
    const uint32_t size = SFileGetFileSize(file, nullptr);
    if (size > 8 * 1024 * 1024) {
        SFileCloseFile(file);
        return;
    }
    std::string contents(size, '\0');
    uint32_t bytesRead = 0;
    const bool read = SFileReadFile(file, contents.data(), size, &bytesRead, nullptr, 0) != 0 && bytesRead == size;
    SFileCloseFile(file);
    if (!read)
        return;
    const auto prefix = FarTerrain::Key(std::string(mapName) + "\\map");
    for (size_t begin = 0; begin < contents.size();) {
        const size_t end = contents.find_first_of("\r\n", begin);
        const auto line =
            std::string_view(contents).substr(begin, (end == std::string::npos ? contents.size() : end) - begin);
        const size_t tab = line.find('\t');
        if (tab != std::string_view::npos) {
            const auto key = FarTerrain::Key(line.substr(0, tab));
            if (key.compare(0, prefix.size(), prefix) == 0) {
                unsigned x, y;
                int consumed = 0;
                if (std::sscanf(key.c_str() + prefix.size(), "%u_%u.blp%n", &x, &y, &consumed) == 2 &&
                    prefix.size() + consumed == key.size() && x < 64 && y < 64) {
                    const auto name = line.substr(tab + 1);
                    // Translation values are archive basenames, never arbitrary paths.
                    if (!name.empty() && name.size() < 128 && name.find_first_of("\\/:\t") == std::string_view::npos &&
                        name.size() > 4 && FarTerrain::Key(name.substr(name.size() - 4)) == ".blp")
                        m_textureNames[y * 64 + x] = std::string("Textures\\Minimap\\") + std::string(name);
                }
            }
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
}

void CMapLowDetail::BeginCensus(const C3Vector &pos) {
    const auto previous = m_census;
    m_census = {};
    m_census.generation = previous.generation;
    m_census.wdlOpened = previous.wdlOpened;
    m_census.wdlRead = previous.wdlRead;
    m_census.nonzeroOffsets = previous.nonzeroOffsets;
    m_census.resident = previous.resident;
    m_census.malformed = previous.malformed;
    m_census.frame = CWorld::frameCnt;
    m_census.camera = pos;
    if (m_requestFrame != uint32_t(CWorld::frameCnt)) {
        m_requestFrame = CWorld::frameCnt;
        m_frameRequests = 0;
    }
    m_census.setting = FarTerrainEnabled() ? CWorldParam::cvar_landscapeDistance->m_floatValue : 0.f;
    m_census.detailFar = World::s_farClip;
    m_census.horizonNear = FarTerrainEnabled() ? World::s_farClip * .75f : World::s_farClip - 50.f;
    m_census.horizonFar = World::GetHorizonDistance();
    for (const auto &entry : m_textures)
        if (entry.texture) {
            ++m_census.cacheEntries;
            if (entry.texture->m_asyncObject) {
                ++m_census.inFlight;
                m_census.inFlightBytes += entry.texture->m_asyncObject->m_size;
            }
        }
    // Worst-case decoded RGBA mip chain; source bodies have a separate in-flight cap.
    m_census.textureBudgetBytes = uint64_t(m_census.cacheEntries) * 256 * 256 * 4 * 4 / 3;
}

CGxTex *CMapLowDetail::GetFarTexture(CMapAreaLow *area) {
    const unsigned tile = unsigned(area->m_index.y) * 64 + unsigned(area->m_index.x);
    if (tile >= 4096) {
        ++m_census.textureMissing;
        return nullptr;
    }
    if (m_missingTextures[tile] || m_textureNames[tile].empty()) {
        ++m_census.textureMissing;
        return nullptr;
    }
    for (auto &entry : m_textures)
        if (entry.tile == tile && entry.texture) {
            if (entry.texture->m_asyncObject) {
                if (uint32_t(CWorld::frameCnt) - entry.loadedFrame > FarTerrain::PendingFrameLimit) {
                    DestroyFarTexture(entry.texture);
                    entry = {};
                    m_missingTextures.set(tile);
                    ++m_census.textureMissing;
                } else
                    ++m_census.texturePending;
                return nullptr;
            }
            if (!entry.texture->m_gxTex || entry.texture->m_status.m_highestSeverity >= STATUS_ERROR) {
                m_missingTextures.set(tile);
                ++m_census.textureMissing;
                return nullptr;
            }
            // Device recreation discarded native pixels: re-enter the bounded async
            // queue rather than letting the texture callback synchronously reload in Draw.
            if (g_theGxDevicePtr->DeviceApi() == GxApi_Vulkan && entry.texture->m_gxTex->m_needsCreation) {
                DestroyFarTexture(entry.texture);
                entry = {};
                if (!m_scheduling)
                    ++m_census.texturePending;
                return nullptr;
            }
            ++m_census.textureReady;
            return TextureGetGxTex(entry.texture, 0, nullptr);
        }
    if (!m_scheduling) {
        ++m_census.textureBudget;
        return nullptr;
    }
    if (m_requestFrame != CWorld::frameCnt) {
        m_requestFrame = CWorld::frameCnt;
        m_frameRequests = 0;
    }
    unsigned pending = 0;
    for (const auto &entry : m_textures)
        if (entry.texture && entry.texture->m_asyncObject)
            ++pending;
    if (m_frameRequests >= FarTerrain::RequestsPerFrame || pending >= FarTerrain::InFlightLimit) {
        return nullptr;
    }
    TextureEntry *slot = nullptr;
    for (auto &entry : m_textures) {
        if (!entry.texture) {
            slot = &entry;
            break;
        }
        // Pending work is pinned. Completed leases rotate fairly in over-budget views.
        if (!entry.texture->m_asyncObject && uint32_t(CWorld::frameCnt) - entry.loadedFrame > 240 &&
            (!slot || entry.loadedFrame < slot->loadedFrame))
            slot = &entry;
    }
    if (!slot)
        return nullptr;
    ++m_frameRequests;
    ++m_census.textureRequests;
    const auto &name = m_textureNames[tile];
    SFileImpl *file = nullptr;
    BLPHeader header{};
    uint32_t size = 0;
    bool valid = false;
    if (SFileOpenFile(name.c_str(), &file) && file) {
        size = SFileGetFileSize(file, nullptr);
        uint32_t bytesRead = 0;
        valid = size >= sizeof(header) && size <= FarTerrain::MaxTextureFileBytes &&
                SFileReadFile(file, &header, sizeof(header), &bytesRead, nullptr, 0) && bytesRead == sizeof(header) &&
                FarTerrain::ValidTexture(header, size);
        SFileCloseFile(file);
    }
    if (!valid) {
        m_missingTextures.set(tile);
        return nullptr;
    }
    if (slot->texture) {
        DestroyFarTexture(slot->texture);
        --m_census.cacheEntries;
    }
    *slot = {};
    // Explicitly async even when another subsystem temporarily selects synchronous textures.
    // No atlas: terrain UVs must span exactly this source tile.
    char path[260];
    SStrCopy(path, name.c_str(), sizeof(path));
    slot->texture = CreateBlpAsync(OsPathFindExtensionWithDot(path), path, 0,
                                   CGxTexFlags((header.m_hasMips & 15) ? GxTex_LinearMipLinear : GxTex_Linear));
    if (!slot->texture) {
        m_missingTextures.set(tile);
        return nullptr;
    }
    HandleCreate(slot->texture, "FarTerrain");
    slot->texture->m_asyncObject->m_userPostUnloadCallback = reinterpret_cast<void *>(FarTextureFailed);
    slot->texture->m_asyncObject->m_userPostloadCallback = reinterpret_cast<void *>(FarTextureLoaded);
    slot->tile = tile;
    slot->loadedFrame = CWorld::frameCnt;
    ++m_census.inFlight;
    ++m_census.cacheEntries;
    m_census.inFlightBytes += size;
    m_census.textureBudgetBytes = uint64_t(m_census.cacheEntries) * 256 * 256 * 4 * 4 / 3;
    return nullptr;
}

void CMapLowDetail::RecordSubmission(CMapAreaLow *area, uint32_t triangles, bool cached) {
    ++m_census.submittedTiles;
    m_census.triangles += triangles;
    if (cached)
        ++m_census.cachedMeshes;
    else {
        ++m_census.streamedMeshes;
    }
    const unsigned tile = unsigned(area->m_index.y) * 64 + unsigned(area->m_index.x);
    if (tile < 4096)
        m_census.submittedIds.set(tile);
    const auto d = area->m_sphere.c - m_census.camera;
    m_census.maxSubmittedDistance =
        std::max(m_census.maxSubmittedDistance, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
}

// VERIFY: 3763adce auto
// OFFSET: 0x007C06E0
CMapAreaLow::CMapAreaLow() {
    this->m_sphere.r = 0.0f;
    this->m_index.x = 0;
    this->m_index.y = 0;
    this->m_horizonLink.m_prevLink = nullptr;
    this->m_horizonLink.m_next = nullptr;
    this->m_indexCount = 0;
    this->m_block = nullptr;
    this->m_flags = 0;
    this->m_heights = nullptr;
    this->m_holes = nullptr;
}

// VERIFY: 0b659a65 auto
// OFFSET: 0x007CC2C0
CMapLowDetail::CMapLowDetail() {
    this->m_data = nullptr;
    this->m_mapObjNames = nullptr;
    this->m_mapObjNameOffsets = nullptr;
    this->m_mapObjDefs = nullptr;
    this->m_mapObjDefCount = 0;
    this->m_areaOffsets = nullptr;
    memset(this->m_areas, 0, sizeof(this->m_areas));
}

// OFFSET: 0x007CBFE0
void CMapLowDetail::Purge() {
    for (auto& entry : m_textures) { DestroyFarTexture(entry.texture); entry={}; }
    m_census.cacheEntries=0; m_census.inFlight=0; m_census.textureBudgetBytes=0; m_census.inFlightBytes=0;
    for (uint32_t i = 0; i < 4096; i++) {
        CMapAreaLow* areaLow = this->m_areas[i];

        if (areaLow) {
            if (areaLow->m_block) {
                CMap::s_vbbList.Release(areaLow->m_block);
            }

            this->m_areas[i]->m_block = nullptr;
        }
    }
}

// VERIFY: 3b80ddc7 manual
// OFFSET: 0x007CC0B0
void CMapLowDetail::CullHorizonAreas(const C3Vector* pos) {
    BeginCensus(*pos);
    float y = -(pos->y - 17066.666f);
    float x = -(pos->x - 17066.666f) * 0.029999999f;
    int32_t row = CMath::fint_(x - 0.5f);
    y = y * 0.029999999f;
    int32_t col = CMath::fint_(y - 0.5f);
    int32_t radius = static_cast<int32_t>(static_cast<int64_t>(World::GetHorizonDistance() * 0.030000001f));

    int32_t minCol = ((col - radius) >> 4) - 2;
    int32_t minRow = ((row - radius) >> 4) - 2;
    int32_t maxCol = ((col + radius) >> 4) + 2;
    int32_t maxRow = ((radius + row) >> 4) + 2;

    if (minRow < 0) {
        minRow = 0;
    }

    if (minCol < 0) {
        minCol = 0;
    }

    if (maxCol >= 64) {
        maxCol = 63;
    }

    if (maxRow >= 64) {
        maxRow = 63;
    }

    m_census.minRow=minRow; m_census.minCol=minCol; m_census.maxRow=maxRow; m_census.maxCol=maxCol;
    for (int32_t r = minRow; r <= maxRow; r++) {
        for (int32_t c = minCol; c <= maxCol; c++) {
            ++m_census.candidates;
            CMapAreaLow* area = this->m_areas[64 * r + c];

            if (!area) { ++m_census.nullAreas; continue; }
            if (CWorldScene::FrustumCull(&area->m_bbox)) { ++m_census.frustumRejects; continue; }
            if (CWorldOcclusion::QueryVolumes(&area->m_sphere)) { ++m_census.volumeRejects; continue; }

            if (World::s_farClip > 300.0f && CWorldOcclusion::QueryBuffer(&area->m_bbox, 40)) {
                ++m_census.bufferRejects; continue;
            }

            ++m_census.admitted; m_census.admittedIds.set(64*r+c);
            CWorldScene::AddHorizonChunk(area);
        }
    }

    if (FarTerrainEnabled()) {
        // Round-robin admission requests prevent near/list-order starvation. A full
        // visible cache gets a four-second minimum lease (at 60 Hz), not a reload storm.
        m_scheduling=true;
        const unsigned start=m_requestCursor;
        for (unsigned n=0; n<4096 && m_frameRequests<FarTerrain::RequestsPerFrame; ++n) {
            const unsigned tile=(start+n)%4096;
            m_requestCursor=(tile+1)%4096;
            if (!m_census.admittedIds[tile] || !m_areas[tile]->m_indexCount || m_missingTextures[tile] || m_textureNames[tile].empty()) continue;
            bool cached=false;
            for (const auto& entry : m_textures) if (entry.texture && entry.tile==tile) { cached=true; break; }
            if (!cached) GetFarTexture(m_areas[tile]);
        }
        m_scheduling=false;
    }

    for (uint32_t i = 0; i < this->m_mapObjDefList.Count(); i++) {
        CMapObjDef* def = this->m_mapObjDefList.m_data[i];

        if (def->m_owner->m_isGroupLoaded && !CWorldScene::FrustumCull(&def->m_bbox) && !CWorldOcclusion::QueryVolumes(&def->m_sphere)) {
            CWorldScene::AddHorizonMapObjDef(def);
        }
    }
}

// VERIFY: ba76c774 manual
// OFFSET: 0x007CC310
void CMapLowDetail::Load(const char* mapPath, const char* mapName) {
    Unload();
    ++m_census.generation;
    LoadTextureNames(mapName);
    char fileName[256];
    SFileImpl* file = nullptr;

    SStrPrintf(fileName, sizeof(fileName), "%s\\%s.wdl", mapPath, mapName);
    SFileOpenFile(fileName, &file);

    if (!file) {
        return;
    }

    m_census.wdlOpened=true;
    uint32_t size = SFileGetFileSize(file, nullptr);
    if (size<16 || size>16*1024*1024) { ++m_census.malformed; SFileCloseFile(file); return; }
    this->m_data = static_cast<char*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    uint32_t bytesRead=0;
    m_census.wdlRead=SFileReadFile(file, this->m_data, size, &bytesRead, nullptr, 0)!=0 && bytesRead==size;
    SFileCloseFile(file);
    if (!m_census.wdlRead) { SMemFree(m_data,nullptr,0,0); m_data=nullptr; return; }

    if (FarTerrain::Word(m_data)!=0x4d564552 || FarTerrain::Word(m_data+4)!=4 || FarTerrain::Word(m_data+8)!=18) {
        ++m_census.malformed; return;
    }
    // Walk chunk boundaries instead of trusting offsets from unvalidated archive bytes.
    for (size_t offset=0; FarTerrain::Range(size,offset,8);) {
        const uint32_t type=FarTerrain::Word(m_data+offset), bytes=FarTerrain::Word(m_data+offset+4);
        if (!FarTerrain::Range(size,offset+8,bytes)) { ++m_census.malformed; break; }
        char* data=m_data+offset+8;
        if (type==0x4d574d4f) m_mapObjNames=data;
        if (type==0x4d574944 && bytes%4==0) m_mapObjNameOffsets=reinterpret_cast<uint32_t*>(data);
        if (type==0x4d4f4446 && bytes%64==0) { m_mapObjDefs=reinterpret_cast<SMMapObjDef*>(data); m_mapObjDefCount=bytes/64; }
        if (type==0x4d414f46 && bytes==4096*4) m_areaOffsets=reinterpret_cast<uint32_t*>(data);
        offset+=8+bytes;
    }
    if (!m_areaOffsets) { ++m_census.malformed; return; }

    uint32_t index = 0;

    for (uint32_t y = 0; y < 1024; y += 16) {
        for (uint32_t x = 0; x < 1024; x += 16) {
            if (this->m_areaOffsets[index]) {
                ++m_census.nonzeroOffsets;
                const uint32_t offset=m_areaOffsets[index];
                if (!FarTerrain::Range(size,offset,8+545*2) || FarTerrain::Word(m_data+offset)!=0x4d415245 ||
                    FarTerrain::Word(m_data+offset+4)!=545*2) { ++m_census.malformed; ++index; continue; }
                char* mare = this->m_data + this->m_areaOffsets[index];
                const size_t nextOffset=size_t(offset)+8+545*2;
                if (FarTerrain::Range(size,nextOffset,4) && FarTerrain::Word(m_data+nextOffset)==0x4d41484f &&
                    (!FarTerrain::Range(size,nextOffset,40) || FarTerrain::Word(m_data+nextOffset+4)!=32)) {
                    ++m_census.malformed; ++index; continue;
                }
                CMapAreaLow* area = CMap::AllocAreaLow();
                this->m_areas[index] = area;
                ++m_census.resident;
                area->m_heights = reinterpret_cast<int16_t*>(mare + 8);
                char* next = mare + 545*2 + 8;

                int32_t minHeight = 100000;
                int32_t maxHeight = -100000;

                for (uint32_t i = 0; i < 545; i++) {
                    int32_t height = area->m_heights[i];

                    if (height > maxHeight) {
                        maxHeight = height;
                    }

                    if (height < minHeight) {
                        minHeight = height;
                    }
                }

                area->m_index.x = x >> 4;
                area->m_index.y = y >> 4;

                float top = 17066.666f - static_cast<float>(y) * 33.333332f;
                area->m_topLeft.x = top;
                area->m_bbox.t.x = top;

                float left = static_cast<float>(x) * -33.333332f + 17066.666f;
                area->m_topLeft.y = left;
                area->m_bbox.t.y = left;
                area->m_bbox.t.z = static_cast<float>(maxHeight);
                area->m_bbox.b.x = top - 533.33331f;
                area->m_bbox.b.y = left - 533.33331f;
                area->m_bbox.b.z = static_cast<float>(minHeight);

                area->m_sphere.c.x = (area->m_bbox.t.x + area->m_bbox.b.x) * 0.5f;
                area->m_sphere.c.y = (area->m_bbox.b.y + area->m_bbox.t.y) * 0.5f;
                area->m_sphere.c.z = 0.5f * (area->m_bbox.b.z + area->m_bbox.t.z);
                float dz = area->m_bbox.t.z - area->m_sphere.c.z;
                float dy = area->m_bbox.t.y - area->m_sphere.c.y;
                float dx = area->m_bbox.t.x - area->m_sphere.c.x;
                area->m_sphere.r = sqrtf(dx * dx + dy * dy + dz * dz);

                if (FarTerrain::Range(size,next-m_data,8+32) && FarTerrain::Word(next) == 0x4D41484F && FarTerrain::Word(next+4)==32) {
                    uint16_t* holes = reinterpret_cast<uint16_t*>(next + 8);
                    area->m_holes = holes;
                    area->m_indexCount = 0;

                    for (uint32_t row = 0; row < 16; row++) {
                        for (uint32_t bit = 0; bit < 16; bit++) {
                            if (!((1 << bit) & holes[row])) {
                                area->m_indexCount += 12;
                            }
                        }
                    }
                } else {
                    area->m_holes = nullptr;
                    area->m_indexCount = 3072;
                }
            }

            index++;
        }
    }

    if (!m_mapObjNames || !m_mapObjNameOffsets || !m_mapObjDefs) m_mapObjDefCount=0;
    C3Vector center(17066.666f, 17066.666f, 0.0f);
    this->m_mapObjDefList.SetCount(this->m_mapObjDefCount);

    for (uint32_t i = 0; i < this->m_mapObjDefCount; i++) {
        SMMapObjDef* def = &this->m_mapObjDefs[i];
        CMapObjDef* mapObjDef = CMap::CreateMapObjDef(this->m_mapObjNames + this->m_mapObjNameOffsets[def->m_nameId], def, &center, false);

        if (!CMap::s_isStreamingMode) {
            CMapObj* mapObj = mapObjDef->m_owner;

            if (!mapObj->m_isGroupLoaded) {
                mapObj->WaitLoad();
            }

            uint32_t groupCount = mapObj->m_groupInfoCount;

            for (uint32_t group = 0; group < groupCount; group++) {
                if (!mapObj->IsGroupLoaded(group)) {
                    mapObj->ReadGroup(group);
                    mapObj->WaitLoadGroup(group);
                }
            }
        }

        mapObjDef->m_refCount++;
        this->m_mapObjDefList.m_data[i] = mapObjDef;
    }
}

namespace {
// Render states are stack-tracked at every nesting level; this also restores inherited
// programs, all eight material stages, lighting and depth state on every return.
class RootRenderStateGuard {
    CGxDevice &m_device;
    CBoundingBox m_viewport;
    C44Matrix m_projection;

  public:
    explicit RootRenderStateGuard(CGxDevice &device)
        : m_device(device), m_viewport(device.m_viewport), m_projection(device.m_projection) {
        m_device.RsPush();
    }
    ~RootRenderStateGuard() {
        m_device.RsPop();
        if (std::memcmp(&m_viewport, &m_device.m_viewport, sizeof(m_viewport)))
            m_device.XformSetViewport(m_viewport.m_x.m_l, m_viewport.m_x.m_h, m_viewport.m_y.m_l, m_viewport.m_y.m_h,
                                      m_viewport.m_z.m_l, m_viewport.m_z.m_h);
        if (m_projection != m_device.m_projection)
            m_device.XformSetProjection(m_projection);
    }
};
bool RenderFarLowDetail(CMapAreaLow *area) {
    auto &low = CMap::s_lowDetail;
    if (!low.FarTerrainEnabled())
        return false;
    auto &device = *g_theGxDevicePtr;
    RootRenderStateGuard guard(device);
    if(FarAdtRenderTile(area)) return true;
    CGxTex *texture = low.GetFarTexture(area);
    if (!texture) {
        ++low.m_census.stockFallback;
        return false;
    }
    device.RsSet(GxRs_VertexShader, static_cast<void *>(nullptr));
    device.RsSet(GxRs_PixelShader, static_cast<void *>(nullptr));
    for (unsigned stage = 0; stage < 8; ++stage)
        device.RsSet(static_cast<EGxRenderState>(GxRs_Texture0 + stage), static_cast<void *>(nullptr));
    device.RsSet(GxRs_Texture0, texture);
    device.RsSet(GxRs_ColorOp0, 0);
    device.RsSet(GxRs_AlphaOp0, 3);
    device.RsSet(GxRs_TextureCoord0, 0);
    device.RsSet(GxRs_TextureShader0, 0);
    device.RsSet(GxRs_TexGen0, 0);
    device.RsSet(GxRs_AlphaRef, 0);
    device.RsSet(GxRs_BlendingMode, 0);
    device.RsSet(GxRs_Lighting, 0);
    device.RsSet(GxRs_DepthTest, 3);
    device.RsSet(GxRs_DepthWrite, 1);
    device.RsSet(GxRs_DepthFunc, 0);
    device.RsSet(GxRs_Culling, 0);
    const auto &info = *DayNight::GetInfo();
    device.RsSet(GxRs_Fog, 1);
    device.RsSet(GxRs_FogStart, info.m_fog.m_start);
    device.RsSet(GxRs_FogEnd, info.m_fog.m_end);
    int32_t fog;
    std::memcpy(&fog, &info.m_fog.m_color, 4);
    device.RsSet(GxRs_FogColor, fog);
    auto *vertices = device.BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPCT), 545);
    auto *data = reinterpret_cast<CGxVertexPCT *>(device.BufLock(vertices));
    for (unsigned i = 0; i < 545; ++i)
        FarTerrain::Vertex(data[i], area->m_topLeft, area->m_heights, i, info.m_light0.m_dir, info.m_light0.m_ambient,
                           info.m_light0.m_diffuse);
    device.BufUnlock(vertices, 0);
    vertices->m_hasData = 1;
    GxPrimVertexPtr(vertices, GxVBF_PCT);
    CGxBuf *indices;
    if (!area->m_holes && area->m_indexCount == 3072) {
        indices = CMap::lowDetailIndexBuf;
        if (!indices->m_hasData || !indices->m_valid)
            CMap::BuildLowDetailGridIndices(nullptr, 0, indices);
    } else {
        indices = device.BufStream(GxPoolTarget_Index, 2, area->m_indexCount);
        CMap::BuildLowDetailGridIndices(area->m_holes, 0, indices);
    }
    device.PrimIndexPtr(indices);
    CGxBatch batch{};
    batch.m_primType = GxPrim_Triangles;
    batch.m_count = area->m_indexCount;
    batch.m_maxIndex = 544;
    device.Draw(&batch, 1);
    low.RecordSubmission(area, area->m_indexCount / 3, false);
    low.m_census.streamedVertexBytes += 545 * sizeof(CGxVertexPCT);
    ++low.m_census.texturedTiles;
    low.m_census.texturedTriangles += area->m_indexCount / 3;
    low.m_census.texturedIds.set(unsigned(area->m_index.y) * 64 + unsigned(area->m_index.x));
    const auto delta = area->m_sphere.c - low.m_census.camera;
    low.m_census.maxTexturedDistance = std::max(low.m_census.maxTexturedDistance,
                                                std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
    return true;
}
} // namespace

// VERIFY: c20afbce manual
// OFFSET: 0x007D5E70
void CMap::RenderLowDetail(CMapAreaLow* area) {
    ++s_lowDetail.m_census.renderCalls;
    if (static_cast<int32_t>(area->m_indexCount) <= 0) {
        ++s_lowDetail.m_census.empty; return;
    }
    if (RenderFarLowDetail(area)) return;

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Fog, 1);
    GxRsSet(GxRs_DepthTest, 3);

    DayNight::DNInfo* info = DayNight::GetInfo();
    GxRsSet(GxRs_FogStart, 0.0f);
    GxRsSet(GxRs_FogEnd, 1.0f);
    CImVector color = info->m_fog.m_color;
    GxRsSet(GxRs_FogColor, *reinterpret_cast<int32_t*>(&color));
    GxRsSet(GxRs_Lighting, 0);

    if (!area->m_block) {
        CMap::s_vbbList.AcquireBlock(&area->m_block);
    }

    if (area->m_block) {
        CGxBuf* buf = area->m_block->m_buf;

        if (!buf->m_hasData || !buf->m_valid) {
            CMap::BuildLowDetailGridVertices(area, buf);
        }

        area->m_block->m_lastUsedFrame = CWorld::frameCnt;
        GxPrimVertexPtr(area->m_block->m_buf, GxVBF_PC);
    } else {
        CGxBuf* buf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, 16, 545);
        CMap::BuildLowDetailGridVertices(area, buf);
        GxPrimVertexPtr(buf, GxVBF_PC);
    }

    uint32_t count;

    if (!(area->m_flags & 0x1) && area->m_indexCount == 3072) {
        GxRsSet(GxRs_Culling, 0);
        CGxBuf* indexBuf = CMap::lowDetailIndexBuf;

        if (!indexBuf->m_hasData || !indexBuf->m_valid) {
            CMap::BuildLowDetailGridIndices(nullptr, 0, CMap::lowDetailIndexBuf);
            indexBuf = CMap::lowDetailIndexBuf;
        }

        g_theGxDevicePtr->PrimIndexPtr(indexBuf);
        count = 3072;
    } else {
        GxRsSet(GxRs_Culling, 0);
        CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, area->m_indexCount);
        CMap::BuildLowDetailGridIndices(area->m_holes, 0, indexBuf);
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);

        CGxBatch batch;
        batch.m_primType = GxPrim_Triangles;
        batch.m_start = 0;
        batch.m_count = area->m_indexCount;
        batch.m_minIndex = 0;
        batch.m_maxIndex = 544;
        g_theGxDevicePtr->Draw(&batch, 1);

        GxRsSet(GxRs_Culling, 1);
        indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, 3072 - area->m_indexCount);
        CMap::BuildLowDetailGridIndices(area->m_holes, 1, indexBuf);
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);
        count = 3072 - area->m_indexCount;
    }

    CGxBatch batch;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_count = count;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 544;
    g_theGxDevicePtr->Draw(&batch, 1);

    s_lowDetail.RecordSubmission(area,1024,area->m_block!=nullptr);
    if (!area->m_block) s_lowDetail.m_census.streamedVertexBytes+=545*sizeof(CGxVertexPC);
    g_theGxDevicePtr->RsPop();
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();
}

// VERIFY: 894a0529 manual
// OFFSET: 0x007D5150
void CMap::BuildLowDetailGridVertices(CMapAreaLow* area, CGxBuf* buf) {
    CGxVertexPC* vertex = reinterpret_cast<CGxVertexPC*>(g_theGxDevicePtr->BufLock(buf));
    uint32_t index = 0;
    float x = area->m_topLeft.x;

    for (int32_t row = 0; row < 17; row++) {
        float y = area->m_topLeft.y;

        for (int32_t col = 0; col < 17; col++) {
            vertex->p.x = x;
            vertex->p.y = y;
            vertex->p.z = area->m_heights[index++];
            vertex->c = CImVector(0xFF, 0xFF, 0xFF, 0xFF);
            vertex++;
            y -= 33.333332f;
        }

        x -= 33.333332f;
    }

    x = area->m_topLeft.x - 16.666666f;

    for (int32_t row = 0; row < 16; row++) {
        float y = area->m_topLeft.y - 16.666666f;

        for (int32_t col = 0; col < 16; col++) {
            vertex->p.x = x;
            vertex->p.y = y;
            vertex->p.z = area->m_heights[index++];
            vertex->c = CImVector(0xFF, 0xFF, 0xFF, 0xFF);
            vertex++;
            y -= 33.333332f;
        }

        x -= 33.333332f;
    }

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// VERIFY: 7d2e0b07 manual
// OFFSET: 0x007D5240
void CMap::BuildLowDetailGridIndices(uint16_t* holes, int32_t pass, CGxBuf* buf) {
    uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(buf));
    uint32_t outer = 0;
    uint32_t inner = 289;

    for (uint32_t row = 0; row < 16; row++) {
        for (int32_t col = 0; col < 16; col++) {
            if (!holes || (pass ? (holes[row] & (1 << col)) != 0 : (holes[row] & (1 << col)) == 0)) {
                *indices++ = inner;
                *indices++ = outer + 1;
                *indices++ = outer;
                *indices++ = inner;
                *indices++ = outer + 18;
                *indices++ = outer + 1;
                *indices++ = inner;
                *indices++ = outer + 17;
                *indices++ = outer + 18;
                *indices++ = inner;
                *indices++ = outer;
                *indices++ = outer + 17;
            }

            outer++;
            inner++;
        }

        outer++;
    }

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// VERIFY: f803b515 manual
// OFFSET: 0x007CC770
void CMapLowDetail::Unload() {
    for (auto& entry : m_textures) {
        if (entry.texture) DestroyFarTexture(entry.texture);
        entry={};
    }
    for (auto& name : m_textureNames) name.clear();
    m_missingTextures.reset(); m_requestCursor=0; m_scheduling=false;
    const uint32_t generation=m_census.generation;
    m_census={}; m_census.generation=generation;
    for (uint32_t i = 0; i < 4096; i++) {
        if (this->m_areas[i]) {
            CMap::FreeAreaLow(this->m_areas[i]);
            this->m_areas[i] = nullptr;
        }
    }

    for (uint32_t i = 0; i < this->m_mapObjDefList.m_count; i++) {
        this->m_mapObjDefList.m_data[i]->m_refCount--;
        CMap::PurgeMapObjDef(this->m_mapObjDefList.m_data[i]);
    }

    this->m_mapObjDefList.m_count = 0;
    this->m_mapObjNames = nullptr;
    this->m_mapObjNameOffsets = nullptr;
    this->m_mapObjDefs = nullptr;
    this->m_mapObjDefCount = 0;
    this->m_areaOffsets = nullptr;

    if (this->m_data) {
        SMemFree(this->m_data, nullptr, 0, 0);
        this->m_data = nullptr;
    }
}
