#include "WoW/Source/WorldClient/TerrainHitSnapshot.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/Map.h"
#include "Engine/Source/Tempest/C3Ray.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/NTempest.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
struct Fixture {
    CMapChunk chunk;SMChunk header{};float heights[145]{};
    Fixture() {
        chunk.m_header=&header;chunk.m_height=heights;chunk.m_topLeftCoords=C3Vector(100,200,300);
        for(unsigned i=0;i<145;++i)heights[i]=float(i%9)*.3f+float(i/9)*.1f;
    }
    ~Fixture(){chunk.m_header=nullptr;chunk.m_height=nullptr;}
};
C3Vector WorldPoint(float x,float y,float z){return C3Vector(17066.666f-y*4.1666665f,17066.666f-x*4.1666665f,z);}
std::vector<TerrainHitCell> OriginalCells(const C3Vector& a,const C3Vector& b) {
    CMap::scCollideList.SetCount(2048);CMap::cCount=0;
    C3Vector start(-(a.y-17066.666f),-(a.x-17066.666f),0),end(-(b.y-17066.666f),-(b.x-17066.666f),0);
    CiRect cells;cells.minX=CMath::fint_(start.x*.24f-.5f);cells.minY=CMath::fint_(start.y*.24f-.5f);
    cells.maxX=CMath::fint_(end.x*.24f-.5f);cells.maxY=CMath::fint_(end.y*.24f-.5f);
    const float dx=end.x-start.x,dy=end.y-start.y;
    if(std::fabs(dx)<.00000023841858f || cells.minX==cells.maxX)CMap::VectorIntersectSY(cells);
    else if(std::fabs(dy)<.00000023841858f || cells.minY==cells.maxY)CMap::VectorIntersectSX(cells);
    else if(std::fabs(dy)>=std::fabs(dx))CMap::VectorIntersectDY(start,end,cells);
    else CMap::VectorIntersectDX(start,end,cells);
    std::vector<TerrainHitCell> result;
    for(uint32_t i=0;i<CMap::cCount;i+=2)result.push_back({int32_t(CMap::scCollideList.m_data[i]),int32_t(CMap::scCollideList.m_data[i+1])});return result;
}
bool OriginalLiquid(const CChunkLiquid& source,uint32_t x,uint32_t y,const C3Ray& ray,float& distance) {
    if(!source.TileExists(x,y))return false;
    const uint32_t row=source.m_tileEnd.y-source.m_tileBegin.y+1;
    const uint32_t base=x+row*(y-source.m_tileBegin.x)-source.m_tileBegin.y,pattern[6]{0,row+1,row,0,1,row+1};bool updated=false;
    for(unsigned i=0;i<2;++i) {
        const uint32_t triangle[3]{base+pattern[3*i],base+pattern[3*i+1],base+pattern[3*i+2]};float value=0;
        if(NTempest::Intersect(ray,source.m_verts,triangle,&value,nullptr,.0099999998f) && value>=0 && value<distance){distance=value;updated=true;}
    }
    return updated;
}
C3Ray Ray(unsigned x,unsigned y,float offset=.5f) {
    C3Ray ray;ray.origin=C3Vector(-(y+offset)*4.1666665f,-(x+offset)*4.1666665f,100);ray.dir=C3Vector(0,0,-1);return ray;
}
}
int main() {
    CMapChunk::InitializeVertexGrid();
    for(float dx:{-60.6f,-2.3f,0.f,2.3f,60.6f})for(float dy:{-60.6f,-2.3f,0.f,2.3f,60.6f}) {
        const auto start=WorldPoint(80.3f,90.7f,100),end=WorldPoint(80.3f+dx,90.7f+dy,-100);
        const auto original=OriginalCells(start,end),owned=TerrainHitCells(start,end);assert(original.size()==owned.size());
        for(size_t i=0;i<original.size();++i)assert(original[i].x==owned[i].x && original[i].y==owned[i].y);
    }
    const auto longRay=TerrainHitCells(WorldPoint(10.3f,10.7f,100),WorldPoint(8000.3f,3000.7f,-100));
    assert(longRay.size()>2048 && longRay.back().x>=7999); // no legacy fixed-scratch truncation
    std::shared_ptr<const TerrainHitFrame> retainedFrame;
    const auto frameStart=WorldPoint(.5f,.5f,400),frameEnd=WorldPoint(20.5f,.5f,290);
    {
        Fixture first,gap,last;first.header.m_holes=gap.header.m_holes=UINT16_MAX;
        first.chunk.m_topLeftCoords=WorldPoint(0,0,300);gap.chunk.m_topLeftCoords=WorldPoint(8,0,300);last.chunk.m_topLeftCoords=WorldPoint(16,0,300);
        const auto a=first.chunk.CaptureHitSnapshot(),b=gap.chunk.CaptureHitSnapshot(),c=last.chunk.CaptureHitSnapshot();
        const auto missing=TerrainHitFrame::Create({{0,0,a},{2,0,c}});float fraction=1;
        assert(!missing->Intersect(frameStart,frameEnd,fraction) && fraction==1);
        retainedFrame=TerrainHitFrame::Create({{0,0,a},{1,0,b},{2,0,c}});
        fraction=1;assert(retainedFrame->Intersect(frameStart,frameEnd,fraction) && fraction>0 && fraction<1);
        float clipped=fraction*.5f;assert(!retainedFrame->Intersect(frameStart,frameEnd,clipped));
    }
    float retainedFraction=1;assert(retainedFrame->Intersect(frameStart,frameEnd,retainedFraction));
    std::shared_ptr<const LiquidHitSnapshot> retainedLiquid;
    for(unsigned begin:{0u,2u}) {
        CChunkLiquid liquid;uint8_t tiles[64];for(auto& tile:tiles)tile=15;
        liquid.m_type=1;liquid.m_tiles=tiles;liquid.m_tileBegin={int32_t(begin),int32_t(begin)};liquid.m_tileEnd={int32_t(8-begin),int32_t(8-begin)};
        const unsigned row=9-2*begin;
        for(unsigned y=begin;y<8-begin;++y)for(unsigned x=begin;x<8-begin;++x)tiles[y*8+x]=0;
        for(unsigned y=0;y<row;++y)for(unsigned x=0;x<row;++x)liquid.m_verts[y*row+x]=C3Vector(-(y+begin)*4.1666665f,-(x+begin)*4.1666665f,20);
        const auto snapshot=LiquidHitSnapshot::Capture(liquid,false);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
            const auto ray=Ray(x,y);float original=1000,owned=1000;
            assert(OriginalLiquid(liquid,x,y,ray,original)==snapshot->Intersect(x,y,ray,owned,false) && original==owned);
            owned=1000;assert(!snapshot->Intersect(x,y,ray,owned,true));
        }
        if(!begin) {
            retainedLiquid=snapshot;Fixture chunk;chunk.chunk.m_topLeftCoords=WorldPoint(0,0,300);
            const auto frame=TerrainHitFrame::Create({{0,0,chunk.chunk.CaptureHitSnapshot(),{snapshot}}});
            const auto start=WorldPoint(.5f,.5f,400),end=WorldPoint(.5f,.5f,290);float terrain=1,water=1,all=1,restricted=1;
            assert(frame->Intersect(start,end,terrain,0x100) && frame->Intersect(start,end,water,0x20000) && frame->Intersect(start,end,all,0x30100));
            assert(water<terrain && all==water && !frame->Intersect(start,end,restricted,0x10000));
            const auto flagged=LiquidHitSnapshot::Capture(liquid,true);float value=1000;assert(flagged->Intersect(0,0,Ray(0,0),value,true));
            char bits[8];for(auto& bit:bits)bit=char(0xff);liquid.m_tiles=nullptr;liquid.m_tileBits.SetRawData(bits,64,false);
            value=1000;assert(LiquidHitSnapshot::Capture(liquid,false)->Intersect(0,0,Ray(0,0),value,false));
            liquid.m_tileBits.SetRawData(bits,1,false);bool rejected=false;
            try{LiquidHitSnapshot::Capture(liquid,false);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
            liquid.m_tiles=tiles;
            float unchanged=1;rejected=false;try{frame->Intersect(start,end,unchanged,0x1000000);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
        }
        for(auto& tile:tiles)tile=15;float value=1000;assert(!LiquidHitSnapshot::Capture(liquid,false)->Intersect(begin,begin,Ray(begin,begin),value,false));
        liquid.m_tiles=nullptr;
    }
    float liquidHit=1000;assert(retainedLiquid->Intersect(0,0,Ray(0,0),liquidHit,false));
    std::shared_ptr<const TerrainHitSnapshot> retained;
    {
        Fixture source;
        for(unsigned mask=0;mask<=16;++mask) {
            source.header.m_holes=mask?uint16_t(1u<<(mask-1)):0;
            const auto snapshot=source.chunk.CaptureHitSnapshot();
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)for(float offset:{.25f,.5f,.75f}) {
                const auto ray=Ray(x,y,offset);float original=std::numeric_limits<float>::max(),frozen=original;
                const auto hit=source.chunk.Intersect(x,y,ray,&original);
                assert(bool(hit)==snapshot->Intersect(x,y,ray,frozen) && original==frozen);
                float clipped=1,copy=1;
                assert(bool(source.chunk.Intersect(x,y,ray,&clipped))==snapshot->Intersect(x,y,ray,copy) && clipped==copy);
            }
            assert(source.chunk.CaptureHitSnapshot()==snapshot); // original queries only changed global z scratch
        }
        source.header.m_holes=0;retained=source.chunk.CaptureHitSnapshot();
        const auto ray=Ray(0,0);float before=1000;assert(retained->Intersect(0,0,ray,before));
        source.heights[9]+=10;
        const auto replacement=source.chunk.CaptureHitSnapshot();assert(replacement!=retained);
        float after=1000,again=1000;assert(replacement->Intersect(0,0,ray,after) && retained->Intersect(0,0,ray,again));
        assert(before==again && before!=after);
        source.chunk.m_topLeftCoords.z+=1;assert(source.chunk.CaptureHitSnapshot()!=replacement);
        assert(retained->Origin().z==300);
        source.heights[0]=std::numeric_limits<float>::infinity();
        bool rejected=false;try{source.chunk.CaptureHitSnapshot();}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    const auto ray=Ray(0,0);float t=1000;assert(retained->Intersect(0,0,ray,t));
    for(auto& vertex:CMapChunk::vertexList)vertex.z=-999;
    t=1000;assert(retained->Intersect(0,0,ray,t));
    for(const auto& vertex:CMapChunk::vertexList)assert(vertex.z==-999);
    const auto mask=CMap::s_holeMask[0];CMap::s_holeMask[0]=UINT32_MAX;
    const auto fan=CMap::s_fanIndices[0];CMap::s_fanIndices[0]=999;
    t=1000;assert(retained->Intersect(0,0,ray,t)); // no global topology read after capture
    {
        Fixture source;bool rejected=false;
        try{source.chunk.CaptureHitSnapshot();}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    CMap::s_holeMask[0]=mask;CMap::s_fanIndices[0]=fan;
    bool rejected=false;try{retained->Intersect(8,0,ray,t);}catch(const std::out_of_range&){rejected=true;}assert(rejected);
    auto invalid=ray;invalid.origin.x=std::numeric_limits<float>::quiet_NaN();rejected=false;
    try{retained->Intersect(0,0,invalid,t);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    std::puts("PASS terrain/liquid collision ownership: original cell-order/fan/hole/tile/flag parity; untruncated long rays; first-missing-chunk termination; source replacement/unload; no global scratch writes; malformed/allocation/finite-data guards");
}
