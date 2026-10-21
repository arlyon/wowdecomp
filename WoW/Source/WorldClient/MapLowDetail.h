#ifndef WOW_SOURCE_WORLDCLIENT_MAPLOWDETAIL_H
#define WOW_SOURCE_WORLDCLIENT_MAPLOWDETAIL_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <bitset>
#include <string>
#include "WoW/Source/WorldClient/FarTerrain.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "WoW/Source/WorldClient/FVBBList.h"

class CTexture;
class CGxTex;
class CMapObjDef;

struct FarTerrainCensus {
    uint32_t generation = 0, frame = 0;
    bool wdlOpened = false, wdlRead = false;
    uint32_t nonzeroOffsets = 0, resident = 0, malformed = 0;
    float setting = 0, detailFar = 0, horizonNear = 0, horizonFar = 0;
    int32_t minRow = 0, minCol = 0, maxRow = -1, maxCol = -1;
    uint32_t candidates = 0, nullAreas = 0, frustumRejects = 0, volumeRejects = 0, bufferRejects = 0, admitted = 0;
    uint32_t renderCalls = 0, empty = 0, submittedTiles = 0, triangles = 0, cachedMeshes = 0, streamedMeshes = 0;
    uint32_t texturedTiles = 0, texturedTriangles = 0;
    uint32_t preparedTiles = 0, preparedChunks = 0, preparedTriangles = 0;
    uint32_t preparedBudget = 0, preparedAllocationFailure = 0, preparedUnsupported = 0, preparedUnavailable = 0;
    float maxPreparedDistance = 0, minPreparedDistance = 0;
    float maxTexturedDistance = 0;
    std::bitset<4096> texturedIds;
    uint32_t textureReady = 0, texturePending = 0, textureMissing = 0, textureBudget = 0, stockFallback = 0;
    uint32_t textureRequests = 0, cacheEntries = 0, inFlight = 0;
    uint64_t textureBudgetBytes = 0, inFlightBytes = 0, streamedVertexBytes = 0;
    float maxSubmittedDistance = 0;
    C3Vector camera;
    std::bitset<4096> admittedIds, submittedIds;
};
struct SMMapObjDef;

class CMapAreaLow {
    public:
    /* 0000 */ uint32_t m_objectIndex;
    /* 0004 */ CAaBox m_bbox;
    /* 001C */ CAaSphere m_sphere;
    /* 002C */ C3Vector m_topLeft;
    /* 0038 */ C2iVector m_index;
    /* 0040 */ uint32_t m_indexCount;
    /* 0044 */ int16_t* m_heights;
    /* 0048 */ uint16_t* m_holes;
    /* 004C */ TSLink<CMapAreaLow> m_horizonLink;
    /* 0054 */ FVBBList::Block* m_block;
    /* 0058 */ uint32_t m_flags;

    CMapAreaLow();
};

class CMapAreaLowHeap {
    public:
    /* 0000 */ uint32_t m_heapId;

    void Delete(CMapAreaLow* areaLow);
};

class CMapLowDetail {
    public:
    /* 0000 */ char* m_data;
    /* 0004 */ char* m_mapObjNames;
    /* 0008 */ uint32_t* m_mapObjNameOffsets;
    /* 000C */ SMMapObjDef* m_mapObjDefs;
    /* 0010 */ uint32_t m_mapObjDefCount;
    /* 0014 */ uint32_t* m_areaOffsets;
    /* 0018 */ CMapAreaLow* m_areas [4096];
    /* 4018 */ TSGrowableArray<CMapObjDef*> m_mapObjDefList;

    FarTerrainCensus m_census;
    const FarTerrainCensus& GetCensus() const { return m_census; }
    bool FarTerrainEnabled() const;
    CGxTex* GetFarTexture(CMapAreaLow* area);
    void RecordSubmission(CMapAreaLow* area, uint32_t triangles, bool cached);

    CMapLowDetail();
    void CullHorizonAreas(const C3Vector* pos);
    void Load(const char* mapPath, const char* mapName);
    void Purge();
    void Unload();

    private:
    struct TextureEntry {
        CTexture* texture = nullptr;
        uint32_t tile = 4096, loadedFrame = 0;
    };
    std::array<TextureEntry, FarTerrain::TextureCapacity> m_textures{};
    std::array<std::string,4096> m_textureNames{};
    std::bitset<4096> m_missingTextures;
    uint32_t m_requestFrame = ~0u, m_frameRequests = 0, m_requestCursor = 0;
    bool m_scheduling = false;
    void LoadTextureNames(const char* mapName);
    void BeginCensus(const C3Vector& pos);
};

#endif
