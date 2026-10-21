#ifndef WOW_SOURCE_WORLDCLIENT_TERRAINHITSNAPSHOT_H
#define WOW_SOURCE_WORLDCLIENT_TERRAINHITSNAPSHOT_H
#include "Engine/Source/Tempest/C3Vector.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
class C3Ray;
class CMapChunk;
class CChunkLiquid;
// Original 145-point collision heightfield, holes and fan topology. It neither
// borrows chunk memory nor writes the legacy global vertex scratch on a query.
class TerrainHitSnapshot {
    std::array<C3Vector,145> vertices;
    std::array<uint32_t,16> masks;
    std::array<int32_t,8> fan;
    C3Vector origin;
    uint16_t holes=0;
    TerrainHitSnapshot()=default;
    bool Matches(const CMapChunk&) const;
    friend class CMapChunk;
public:
    static std::shared_ptr<const TerrainHitSnapshot> Capture(const CMapChunk&);
    const C3Vector& Origin() const {return origin;}
    bool Intersect(uint32_t subX,uint32_t subY,const C3Ray& localRay,float& t) const;
};
class LiquidHitSnapshot {
    std::array<C3Vector,81> vertices;
    std::array<std::array<uint8_t,6>,64> indices;
    std::array<bool,64> present{};
    bool typeFlag4=false;
    LiquidHitSnapshot()=default;
public:
    static std::shared_ptr<const LiquidHitSnapshot> Capture(const CChunkLiquid&,bool typeFlag4);
    bool Intersect(uint32_t subX,uint32_t subY,const C3Ray& localRay,float& distance,bool requireTypeFlag4) const;
};
struct TerrainHitCell {int32_t x,y;};
std::vector<TerrainHitCell> TerrainHitCells(const C3Vector& start,const C3Vector& end);
struct TerrainHitFrameChunk {
    uint32_t x,y;
    std::shared_ptr<const TerrainHitSnapshot> geometry;
    std::vector<std::shared_ptr<const LiquidHitSnapshot>> liquids;
};
// Original terrain/liquid paths ONLY. Doodad/entity M2 and WMO collision are
// separate producers, not silently certified by this loaded-chunk snapshot.
class TerrainHitFrame {
    struct Chunk {
        std::shared_ptr<const TerrainHitSnapshot> geometry;
        std::vector<std::shared_ptr<const LiquidHitSnapshot>> liquids;
    };
    std::unordered_map<uint32_t,Chunk> chunks;
    TerrainHitFrame()=default;
public:
    static std::shared_ptr<const TerrainHitFrame> Capture();
    static std::shared_ptr<const TerrainHitFrame> Create(const std::vector<TerrainHitFrameChunk>&);
    bool Intersect(const C3Vector& start,const C3Vector& end,float& fraction,uint32_t flags=0x100) const;
};
#endif
