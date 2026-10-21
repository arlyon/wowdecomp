#include "WoW/Source/WorldClient/WmoGeometryHitSnapshot.h"
#include "WoW/Source/WorldClient/WmoLiquidHitSnapshot.h"
#include "WoW/Source/WorldClient/WmoHitFrame.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
namespace {
struct Fixture {
    CMapObj parent;CMapObjGroup group;
    SMOMaterial materials[2]{};SMOPoly faces[2]{{0,0},{0,1}};
    C3Vector vertices[6]{{-1,-1,2},{1,-1,2},{0,1,2},{-1,-1,4},{1,-1,4},{0,1,4}};
    uint16_t indices[6]{0,1,2,3,4,5},leafFaces[4]{0,1,0,1};CAaBspNode nodes[3]{};
    Fixture() {
        parent.m_materialList=materials;parent.m_materialsCount=2;materials[1].m_blendMode=1;
        group.m_parent=&parent;group.m_polyList=faces;group.m_polyListSize=2;group.m_vertexList=vertices;group.m_vertexListCount=6;
        group.m_indices=indices;group.m_indicesCount=6;group.m_flags=0;
        nodes[0].m_flags=0;nodes[0].m_planeDist=0;nodes[0].m_posChild=1;nodes[0].m_negChild=2;
        nodes[1].m_flags=nodes[2].m_flags=4;nodes[1].m_nFaces=nodes[2].m_nFaces=2;nodes[2].m_faceStart=2;
        CAaBox bounds;bounds.b=C3Vector(-10,-10,-10);bounds.t=C3Vector(10,10,10);
        group.m_cAaBspNodePtr1.Set(nodes,3,leafFaces,4,bounds);
    }
    ~Fixture() {
        // Real lifecycle; detach only the fixture's borrowed source arrays.
        group.m_cAaBspNodePtr1.Clear();group.m_parent=nullptr;group.m_polyList=nullptr;group.m_vertexList=nullptr;group.m_indices=nullptr;
        group.m_liquidVertArray=nullptr;group.m_liquidTileList=nullptr;
        parent.m_materialList=nullptr;parent.m_materialsCount=0;parent.m_mapObjGroupCount=0;
    }
    void Compare(C3Segment segment,uint16_t mask,float limit=1) {
        const auto snapshot=group.CaptureGeometryHitSnapshot();float a=limit,b=limit;int32_t original=-1,owned=-1;
        const bool hit=group.Intersect(segment,&a,0,mask,&original);
        const auto tests=BspQuery::testFaceSub,hits=BspQuery::hitFaceSub;const auto first=faces[0].m_flags,second=faces[1].m_flags;
        assert(snapshot->Intersect(segment,b,mask,owned)==hit && a==b && (!hit || original==owned));
        assert(BspQuery::testFaceSub==tests && BspQuery::hitFaceSub==hits && faces[0].m_flags==first && faces[1].m_flags==second);
        assert(group.CaptureGeometryHitSnapshot()==snapshot);
    }
};
C3Segment Segment(float x=0,float y=0){C3Segment s;s.b=C3Vector(x,y,0);s.t=C3Vector(x,y,10);return s;}
}
int main() {
    std::shared_ptr<const WmoLiquidHitSnapshot> retainedLiquid;
    C3Segment liquidSegment;liquidSegment.b=C3Vector(1,1,10);liquidSegment.t=C3Vector(1,1,-1);
    {
        Fixture source;CMapObjGroup::VertArray data;data.m_verts.SetCount(9);SMOLTile tiles[4]{};tiles[3].m_flags=15;
        source.group.m_liquidType=1;source.group.m_liquidTiles={2,2};source.group.m_liquidVerts={3,3};
        source.group.m_liquidVertArray=&data;source.group.m_liquidTileList=tiles;source.group.m_liquidCorner=C3Vector(0,0,0);
        source.group.m_liquidHeight=2;source.group.m_liquidMaxHeight=2.6f;
        for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x)data.m_verts[y*3+x]=C3Vector(x*4.1666665f,y*4.1666665f,2+x*.1f+y*.2f);
        retainedLiquid=WmoLiquidHitSnapshot::Capture(source.group,false);
        for(int x=-1;x<10;++x)for(int y=-1;y<10;++y) {
            C3Segment segment;segment.b=C3Vector(x*.9f,y*.9f,10);segment.t=C3Vector(x*.9f,y*.9f,-1);
            float a=1,b=1;const bool native=source.group.VectorIntersectLiquid(segment,&a,0x20000,nullptr,nullptr);
            assert(retainedLiquid->Intersect(segment,b,0x20000)==native && a==b);
        }
        for(unsigned axis=0;axis<4;++axis) {
            C3Segment segment;segment.b=C3Vector(.2f,.2f,2.3f);segment.t=C3Vector(axis&1?7.8f:.2f,axis&2?7.8f:.2f,2.3f);
            if(!axis)continue;
            for(bool reverse:{false,true})for(float limit:{.1f,.5f,1.f}) {
                auto query=segment;if(reverse)std::swap(query.b,query.t);
                float a=limit,b=limit;const bool native=source.group.VectorIntersectLiquid(query,&a,0x30000,nullptr,nullptr);
                assert(retainedLiquid->Intersect(query,b,0x30000)==native && a==b);
            }
        }
        float t=1;assert(!retainedLiquid->Intersect(liquidSegment,t,0x10000));
        assert(WmoLiquidHitSnapshot::Capture(source.group,true)->Intersect(liquidSegment,t,0x10000));
        data.m_verts[0].z+=10;tiles[0].m_flags=15;t=1;assert(retainedLiquid->Intersect(liquidSegment,t,0x20000));
        data.m_verts.SetCount(8);bool rejected=false;try{WmoLiquidHitSnapshot::Capture(source.group,false);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    float retainedT=1;assert(retainedLiquid->Intersect(liquidSegment,retainedT,0x20000));
    {
        Fixture source;CMapObjGroup::VertArray data;data.m_verts.SetCount(4098);std::vector<SMOLTile> tiles(2048);
        source.group.m_liquidType=1;source.group.m_liquidTiles={2048,1};source.group.m_liquidVerts={2049,2};
        source.group.m_liquidVertArray=&data;source.group.m_liquidTileList=tiles.data();source.group.m_liquidCorner=C3Vector(0,0,0);
        source.group.m_liquidHeight=source.group.m_liquidMaxHeight=2;
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2049;++x)data.m_verts[y*2049+x]=C3Vector(x*4.1666665f,y*4.1666665f,2);
        const auto snapshot=WmoLiquidHitSnapshot::Capture(source.group,true);C3Segment segment;
        segment.b=C3Vector(2047.5f*4.1666665f,1,10);segment.t=C3Vector(segment.b.x,1,0);float t=1;
        assert(snapshot->Intersect(segment,t,0x20000)); // actual indices beyond an8-bit tile coordinate
    }
    std::shared_ptr<const WmoGeometryHitSnapshot> retained;
    {
        Fixture source;
        for(int x=-5;x<=5;++x)for(int y=-5;y<=5;++y)for(uint16_t mask:{uint16_t(0),uint16_t(0x100),uint16_t(0x200),uint16_t(0xffff)})
            source.Compare(Segment(x*.3f,y*.3f),mask);
        source.Compare(Segment(),0,.1f);source.Compare(Segment(),0,.3f);
        for(bool reverse:{false,true}) {
            auto segment=Segment();segment.b.x=-2;segment.t.x=2;if(reverse)std::swap(segment.b,segment.t);
            source.Compare(segment,0);
        }
        retained=source.group.CaptureGeometryHitSnapshot();source.vertices[0].x=100;source.materials[0].m_blendMode=1;source.faces[0].m_flags=2;
        float t=1;int32_t face=-1;assert(retained->Intersect(Segment(),t,0,face) && face==0 && std::abs(t-.2f)<1e-6f);
    }
    float t=1;int32_t face=-1;assert(retained->Intersect(Segment(),t,0,face) && face==0);
    {
        WmoHitFrame::Entry entry;entry.inverse.Identity();entry.inverse.d0=-20;entry.guid=11;entry.groupKey=3;entry.geometry=retained;
        entry.localBounds.b=C3Vector(-10,-10,-10);entry.localBounds.t=C3Vector(10,10,10);
        entry.definitionBounds.b=C3Vector(10,-10,-10);entry.definitionBounds.t=C3Vector(30,10,10);entry.groupBounds=entry.definitionBounds;
        std::vector<WmoHitFrame::Entry> entries(600,entry);entries.back().definitionFlags=0x100;
        const WmoHitFrame frame(entries);auto segment=Segment(20,0);WmoHitFrame::Hit hit;t=1;
        assert(frame.Candidates(segment,0).size()==600 && frame.Candidates(segment,0x100).size()==599);
        assert(frame.Intersect(segment,t,0,0x100,hit) && hit.guid==11 && hit.groupKey==3 && hit.face==0 && std::abs(t-.2f)<1e-6f);
        assert(frame.Candidates(Segment(),0).empty());
        entries[0].inverse.d0=100;entries[0].definitionBounds.b.x=100; // frame owns its transforms/bounds
        t=1;assert(frame.Intersect(segment,t,0,0x100,hit));
    }
    {
        Fixture source;std::vector<SMOPoly> faces(9000,SMOPoly{0,0});std::vector<uint16_t> indices(27000),leaf(9000);
        for(unsigned i=0;i<9000;++i){leaf[i]=i;indices[3*i]=0;indices[3*i+1]=1;indices[3*i+2]=2;}
        source.vertices[3].z=source.vertices[4].z=source.vertices[5].z=1;indices[26997]=3;indices[26998]=4;indices[26999]=5;
        source.group.m_polyList=faces.data();source.group.m_polyListSize=faces.size();source.group.m_indices=indices.data();source.group.m_indicesCount=indices.size();
        source.nodes[0].m_flags=4;source.nodes[0].m_faceStart=0;source.nodes[0].m_nFaces=9000;
        CAaBox bounds;bounds.b=C3Vector(-10,-10,-10);bounds.t=C3Vector(10,10,10);source.group.m_cAaBspNodePtr1.Set(source.nodes,1,leaf.data(),leaf.size(),bounds);
        const auto snapshot=source.group.CaptureGeometryHitSnapshot();t=1;face=-1;
        assert(snapshot->Intersect(Segment(),t,0,face) && face==8999 && std::abs(t-.1f)<1e-6f); // past the legacy8192 scratch limit
    }
    {
        Fixture source;std::vector<CAaBspNode> nodes(20000);
        for(unsigned i=0;i<nodes.size();++i){nodes[i].m_flags=0;nodes[i].m_negChild=0xffff;nodes[i].m_posChild=uint16_t(i+1);nodes[i].m_planeDist=0;}
        nodes.back().m_flags=4;nodes.back().m_faceStart=0;nodes.back().m_nFaces=2;
        CAaBox bounds;bounds.b=C3Vector(-10,-10,-10);bounds.t=C3Vector(10,10,10);source.group.m_cAaBspNodePtr1.Set(nodes.data(),nodes.size(),source.leafFaces,4,bounds);
        const auto snapshot=source.group.CaptureGeometryHitSnapshot();t=1;face=-1;
        assert(snapshot->Intersect(Segment(),t,0,face) && face==0); // iterative, no arbitrary depth ceiling
    }
    for(unsigned invalid=0;invalid<4;++invalid) {
        Fixture source;
        if(invalid==0)source.nodes[0].m_posChild=0;
        if(invalid==1)source.indices[0]=6;
        if(invalid==2)source.faces[0].m_materialId=2;
        if(invalid==3)source.nodes[1].m_faceStart=4;
        bool rejected=false;try{source.group.CaptureGeometryHitSnapshot();}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    auto invalid=Segment();invalid.b.x=std::numeric_limits<float>::infinity();bool rejected=false;
    try{retained->Intersect(invalid,t,0,face);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    std::puts("PASS owned WMO BSP and liquid: original traversal/face/material/clipping/tile/triangle parity; liquid type policy; source mutation/destruction; private scratch; no8192-face or8-bit tile-coordinate limit; graph/index/material/allocation/finite-data safeguards");
}
