#include "TerrainHitSnapshot.h"
#include "MapChunk.h"
#include "Map.h"
#include "MapArea.h"
#include "CChunkLiquid.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include <unordered_set>
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/C3Ray.h"
#include "Engine/Source/Tempest/NTempest.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace {
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool Same(float a,float b){return !std::memcmp(&a,&b,sizeof(float));}
}
std::shared_ptr<const TerrainHitSnapshot> TerrainHitSnapshot::Capture(const CMapChunk& chunk) {
    if(!chunk.m_header || !chunk.m_height || !Finite(chunk.m_topLeftCoords))throw std::invalid_argument("Missing original terrain collision source");
    auto result=std::shared_ptr<TerrainHitSnapshot>(new TerrainHitSnapshot);
    result->origin=chunk.m_topLeftCoords;result->holes=chunk.m_header->m_holes;
    std::copy_n(CMap::s_holeMask,result->masks.size(),result->masks.begin());
    std::copy_n(CMap::s_fanIndices,result->fan.size(),result->fan.begin());
    for(auto offset:result->fan)if(offset<0 || uint64_t(offset)+126>=result->vertices.size())
        throw std::invalid_argument("Original terrain fan exceeds its heightfield");
    for(size_t i=0;i<result->vertices.size();++i) {
        result->vertices[i]=C3Vector(CMapChunk::vertexList[i].x,CMapChunk::vertexList[i].y,chunk.m_height[i]);
        if(!Finite(result->vertices[i]))throw std::invalid_argument("Nonfinite original terrain collision vertex");
    }
    return result;
}
bool TerrainHitSnapshot::Matches(const CMapChunk& chunk) const {
    if(!chunk.m_header || !chunk.m_height || holes!=chunk.m_header->m_holes ||
        !Same(origin.x,chunk.m_topLeftCoords.x) || !Same(origin.y,chunk.m_topLeftCoords.y) || !Same(origin.z,chunk.m_topLeftCoords.z) ||
        !std::equal(masks.begin(),masks.end(),CMap::s_holeMask) || !std::equal(fan.begin(),fan.end(),CMap::s_fanIndices))return false;
    for(size_t i=0;i<vertices.size();++i)
        if(!Same(vertices[i].x,CMapChunk::vertexList[i].x) || !Same(vertices[i].y,CMapChunk::vertexList[i].y) || !Same(vertices[i].z,chunk.m_height[i]))return false;
    return true;
}
bool TerrainHitSnapshot::Intersect(uint32_t subX,uint32_t subY,const C3Ray& ray,float& t) const {
    if(subX>=8 || subY>=8)throw std::out_of_range("Terrain collision cell exceeds original heightfield");
    if(!Finite(ray.origin) || !Finite(ray.dir) || !std::isfinite(t))throw std::invalid_argument("Nonfinite terrain collision query");
    if(holes&masks[4*(subY>>1)+(subX>>1)])return false;
    const uint32_t base=17*subY+subX;bool hit=false;
    for(uint32_t i=0;i<4;++i) {
        const uint32_t triangle[3]{base+9,base+uint32_t(fan[2*i+1]),base+uint32_t(fan[2*i])};float value=0;
        if(NTempest::Intersect(ray,vertices.data(),triangle,&value,nullptr,.0099999998f)) {
            hit=true;if(value<t && value>=0)t=value;
        }
    }
    return hit;
}
std::shared_ptr<const LiquidHitSnapshot> LiquidHitSnapshot::Capture(const CChunkLiquid& source,bool flag4) {
    if(source.m_tileBegin.x<0 || source.m_tileBegin.y<0 || source.m_tileEnd.x>8 || source.m_tileEnd.y>8 ||
        source.m_tileEnd.x<source.m_tileBegin.x || source.m_tileEnd.y<source.m_tileBegin.y)
        throw std::invalid_argument("Invalid original liquid vertex grid");
    auto result=std::shared_ptr<LiquidHitSnapshot>(new LiquidHitSnapshot);result->typeFlag4=flag4;
    const int32_t row=source.m_tileEnd.y-source.m_tileBegin.y+1;
    const int32_t pattern[6]{0,row+1,row,0,1,row+1};
    for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<8;++x) {
        if(!source.m_tiles && x>=uint32_t(source.m_tileBegin.y) && y>=uint32_t(source.m_tileBegin.x) &&
            x<uint32_t(source.m_tileEnd.y) && y<uint32_t(source.m_tileEnd.x)) {
            const uint32_t bit=x+(source.m_tileEnd.y-source.m_tileBegin.y)*(y-source.m_tileBegin.x)-source.m_tileBegin.y;
            if(!source.m_tileBits.m_array || bit>=source.m_tileBits.m_size)throw std::invalid_argument("Original liquid tile exceeds its owned bit allocation");
        }
        const auto cell=y*8+x;result->present[cell]=source.TileExists(x,y);if(!result->present[cell])continue;
        const int32_t base=int32_t(x)+row*(int32_t(y)-source.m_tileBegin.x)-source.m_tileBegin.y;
        for(unsigned n=0;n<6;++n) {
            const int32_t index=base+pattern[n];
            if(index<0 || size_t(index)>=result->vertices.size())throw std::invalid_argument("Original liquid triangle exceeds vertex allocation");
            const auto& vertex=source.m_verts[index];if(!Finite(vertex))throw std::invalid_argument("Nonfinite original liquid collision vertex");
            result->vertices[index]=vertex;result->indices[cell][n]=uint8_t(index);
        }
    }
    return result;
}
bool LiquidHitSnapshot::Intersect(uint32_t x,uint32_t y,const C3Ray& ray,float& distance,bool requireFlag4) const {
    if(x>=8 || y>=8)throw std::out_of_range("Liquid collision cell exceeds original grid");
    if(!Finite(ray.origin) || !Finite(ray.dir) || !std::isfinite(distance))throw std::invalid_argument("Nonfinite liquid collision query");
    const auto cell=y*8+x;if((requireFlag4 && !typeFlag4) || !present[cell])return false;
    bool updated=false;
    for(unsigned i=0;i<2;++i) {
        const uint32_t triangle[3]{indices[cell][3*i],indices[cell][3*i+1],indices[cell][3*i+2]};float value=0;
        if(NTempest::Intersect(ray,vertices.data(),triangle,&value,nullptr,.0099999998f) && value>=0 && value<distance) {
            distance=value;updated=true;
        }
    }
    return updated;
}
std::shared_ptr<const TerrainHitSnapshot> CMapChunk::CaptureHitSnapshot() {
    if(!m_hitSnapshot || !m_hitSnapshot->Matches(*this))m_hitSnapshot=TerrainHitSnapshot::Capture(*this);
    return m_hitSnapshot;
}
namespace {
bool InTerrainGrid(int32_t x,int32_t y){return x>=0 && y>=0 && x<=8192 && y<=8192;}
int32_t GridRound(float value) {
    if(!std::isfinite(value) || double(value)<double(INT32_MIN) || double(value)>double(INT32_MAX))
        throw std::invalid_argument("Terrain traversal coordinate is not representable");
    return CMath::fint_(value);
}
template<class Visitor> void VisitTerrainCells(const C3Vector& start,const C3Vector& end,Visitor visit) {
    if(!Finite(start) || !Finite(end))throw std::invalid_argument("Nonfinite terrain traversal segment");
    const float sx=-(start.y-17066.666f),sy=-(start.x-17066.666f),ex=-(end.y-17066.666f),ey=-(end.x-17066.666f);
    const int32_t ax=GridRound(sx*.24f-.5f),ay=GridRound(sy*.24f-.5f),bx=GridRound(ex*.24f-.5f),by=GridRound(ey*.24f-.5f);
    const auto emit=[&](int32_t x,int32_t y){return visit(x,y) && InTerrainGrid(x,y);};
    if(!InTerrainGrid(ax,ay)){emit(ax,ay);return;}
    const float dx=ex-sx,dy=ey-sy;
    if(std::fabs(dx)<.00000023841858f || ax==bx) {
        const int32_t step=ay<=by?1:-1;
        for(int32_t y=ay;;y+=step){if(!emit(ax,y) || y==by)return;}
    }
    if(std::fabs(dy)<.00000023841858f || ay==by) {
        const int32_t step=ax<=bx?1:-1;
        for(int32_t x=ax;;x+=step){if(!emit(x,ay) || x==bx)return;}
    }
    const float slope=(ey-sy)/(ex-sx),intercept=sy-sx*slope;
    int32_t x=ax,y=ay;
    if(std::fabs(dy)<std::fabs(dx)) {
        const int32_t step=bx<=x?-1:1,next=step<0?x:x+1;float edge=next*4.1666665f;
        if(!emit(x,y))return;
        while(int64_t(x)!=int64_t(bx)+step) {
            const int32_t edgeY=GridRound((slope*edge+intercept)*.24f-.5f);
            if(edgeY!=y && !emit(x,edgeY))return;
            x+=step;edge=step*4.1666665f+edge;
            if(!emit(x,edgeY))return;y=edgeY;
        }
        if(y!=by)emit(bx,by);
    }else {
        const float invSlope=1.f/slope;
        const int32_t step=by<=y?-1:1,next=step<0?y:y+1;float edge=next*4.1666665f;
        if(!emit(x,y))return;
        while(int64_t(y)!=int64_t(by)+step) {
            const int32_t edgeX=GridRound((edge-intercept)*invSlope*.24f-.5f);
            if(edgeX!=x && !emit(edgeX,y))return;
            edge=step*4.1666665f+edge;y+=step;
            if(!emit(edgeX,y))return;x=edgeX;
        }
        if(x!=bx)emit(bx,by);
    }
}
}
std::vector<TerrainHitCell> TerrainHitCells(const C3Vector& start,const C3Vector& end) {
    std::vector<TerrainHitCell> result;
    VisitTerrainCells(start,end,[&](int32_t x,int32_t y){result.push_back({x,y});return true;});return result;
}
std::shared_ptr<const TerrainHitFrame> TerrainHitFrame::Create(const std::vector<TerrainHitFrameChunk>& source) {
    auto result=std::shared_ptr<TerrainHitFrame>(new TerrainHitFrame);
    for(const auto& chunk:source) {
        if(chunk.x>=1024 || chunk.y>=1024 || !chunk.geometry)throw std::invalid_argument("Invalid original loaded terrain chunk identity");
        for(const auto& liquid:chunk.liquids)if(!liquid)throw std::invalid_argument("Missing original liquid collision snapshot");
        if(!result->chunks.emplace(chunk.y*1024+chunk.x,Chunk{chunk.geometry,chunk.liquids}).second)throw std::invalid_argument("Duplicate original loaded terrain chunk");
    }
    return result;
}
std::shared_ptr<const TerrainHitFrame> TerrainHitFrame::Capture() {
    std::vector<TerrainHitFrameChunk> source;
    for(uint32_t area=0;area<4096;++area) {
        auto* loaded=CMap::m_areaTable[area];if(!loaded || loaded->m_asyncObject)continue;
        for(uint32_t i=0;i<256;++i) {
            auto* chunk=loaded->m_mapChunks[i];if(!chunk || !chunk->m_header || !chunk->m_height)continue;
            TerrainHitFrameChunk entry{(area%64)*16+i%16,(area/64)*16+i/16,chunk->CaptureHitSnapshot(),{}};
            std::unordered_set<const CChunkLiquid*> seen;
            for(auto* liquid=chunk->m_liquidChunkLinkList.Head();liquid;liquid=chunk->m_liquidChunkLinkList.Next(liquid)) {
                if(!seen.insert(liquid).second || liquid->m_owner!=chunk)throw std::invalid_argument("Invalid original chunk liquid ownership");
                const auto* type=g_liquidTypeDB.GetRecord(liquid->m_type);
                if(!type)throw std::invalid_argument("Missing original liquid collision type");
                entry.liquids.push_back(LiquidHitSnapshot::Capture(*liquid,(type->m_flags&4)!=0));
            }
            source.push_back(std::move(entry));
        }
    }
    return Create(source);
}
bool TerrainHitFrame::Intersect(const C3Vector& start,const C3Vector& end,float& fraction,uint32_t flags) const {
    if(flags&~0x30100u)throw std::invalid_argument("Terrain/liquid frame cannot certify other world collision producers");
    if(!Finite(start) || !Finite(end) || !std::isfinite(fraction))throw std::invalid_argument("Nonfinite terrain frame query");
    const float dx=end.x-start.x,dy=end.y-start.y,dz=end.z-start.z;
    const float length=std::sqrt(dz*dz+dy*dy+dx*dx);
    if(!std::isfinite(length))throw std::invalid_argument("Terrain frame ray extent overflow");
    if(!length || fraction<=0)return false;
    const float inv=1.f/length;const C3Vector direction(dx*inv,dy*inv,inv*dz);float best=fraction;
    uint32_t previous=UINT32_MAX;const Chunk* current=nullptr;C3Ray local;
    VisitTerrainCells(start,end,[&](int32_t x,int32_t y) {
        if(!InTerrainGrid(x,y))return false;
        const uint32_t key=((uint32_t(y)>>3)&1023)*1024+((uint32_t(x)>>3)&1023);
        if(key!=previous) {
            const auto chunk=chunks.find(key);if(chunk==chunks.end())return false; // original first-missing-chunk termination
            previous=key;current=&chunk->second;local.origin=start-current->geometry->Origin();local.dir=direction;
        }
        float distance=3.4028235e38f;
        if((flags&0x100) && current->geometry->Intersect(uint32_t(x)&7,uint32_t(y)&7,local,distance)) {
            const float value=distance*inv;if(best>value && value>=0)best=value;
        }
        if(flags&0x30000)for(const auto& liquid:current->liquids) {
            distance=3.4028235e38f;
            if(liquid->Intersect(uint32_t(x)&7,uint32_t(y)&7,local,distance,(flags&0x10000) && !(flags&0x20000))) {
                const float value=distance*inv;if(best>value && value>=0)best=value;
            }
        }
        return true;
    });
    if(best>=fraction)return false;fraction=best;return true;
}
