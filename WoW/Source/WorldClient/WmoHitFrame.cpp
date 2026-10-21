#include "WmoHitFrame.h"
#include "Map.h"
#include "MapObj.h"
#include "MapObjDef.h"
#include "MapObjGroup.h"
#include "CWorldMath.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
namespace {
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool Valid(const CAaBox& b){return Finite(b.b)&&Finite(b.t)&&b.b.x<=b.t.x&&b.b.y<=b.t.y&&b.b.z<=b.t.z;}
bool Bounds(const CAaBox& box,const C3Segment& s){auto b=box;auto start=s.b,end=s.t;return CWorldMath::VectorIntersectAABox2(b,start,end);}
float Length(const C3Segment& s){const auto d=s.t-s.b;return std::sqrt(d.x*d.x+(d.y*d.y+d.z*d.z));}
}
WmoHitFrame::WmoHitFrame(std::vector<Entry> source):entries(std::move(source)) {
    for(const auto& e:entries) {
        if(!Valid(e.definitionBounds)||!Valid(e.groupBounds)||!Valid(e.localBounds))throw std::invalid_argument("Invalid original WMO instance bounds");
        const auto& m=e.inverse;
        for(float value:{m.a0,m.a1,m.a2,m.a3,m.b0,m.b1,m.b2,m.b3,m.c0,m.c1,m.c2,m.c3,m.d0,m.d1,m.d2,m.d3})
            if(!std::isfinite(value))throw std::invalid_argument("Nonfinite original WMO instance inverse");
    }
}
std::shared_ptr<const WmoHitFrame> WmoHitFrame::Capture() {
    std::vector<Entry> source;
    for(auto* def=CMap::mapObjDefHash.m_fulllist.Head();def;def=CMap::mapObjDefHash.m_fulllist.Next(def)) {
        auto* owner=def->m_owner;if(!owner || !owner->m_isGroupLoaded)continue;
        const auto count=def->m_defGroups.m_count==UINT32_MAX?def->m_defGroups.m_dynamic.m_count:def->m_defGroups.m_count;
        if(owner->m_groupInfoCount<0 || count>uint32_t(owner->m_groupInfoCount) || count>std::size(owner->m_mapObjGroupArray) || (count && !owner->m_groupInfo))
            throw std::invalid_argument("Original WMO definition exceeds group allocations");
        for(uint32_t i=0;i<count;++i) {
            auto* group=owner->m_mapObjGroupArray[i];if(!group || !(group->m_unkLoadedFlag&1))continue;
            const auto* instance=def->m_defGroups[i];if(!instance || instance->m_groupNum!=i)throw std::invalid_argument("Original WMO group instance identity mismatch");
            Entry e;e.definitionBounds=def->m_bbox;e.groupBounds=instance->m_bbox;e.localBounds=owner->m_groupInfo[i].m_boundingBox;
            e.inverse=def->m_invMat;e.definitionFlags=def->m_flags;e.groupKey=uint32_t(instance->m_objectIndex);e.guid=def->m_objectGuid.m_guid;
            e.geometry=group->CaptureGeometryHitSnapshot();e.liquid=group->CaptureLiquidHitSnapshot();source.push_back(std::move(e));
        }
    }
    return std::make_shared<const WmoHitFrame>(std::move(source));
}
std::vector<WmoHitFrame::Candidate> WmoHitFrame::Candidates(const C3Segment& segment,uint32_t ignore) const {
    if(!Finite(segment.b)||!Finite(segment.t))throw std::invalid_argument("Nonfinite WMO instance query");
    const float length=Length(segment);if(!std::isfinite(length))throw std::invalid_argument("WMO instance query extent overflow");
    std::vector<Candidate> result;if(!length)return result;
    for(size_t index=0;index<entries.size();++index) {
        const auto& e=entries[index];if((e.definitionFlags&ignore) || !Bounds(e.definitionBounds,segment) || !Bounds(e.groupBounds,segment))continue;
        const auto local=segment.b*e.inverse;if(!Finite(local))throw std::invalid_argument("WMO local candidate origin overflow");
        float fraction=0;
        if(!e.localBounds.ContainsPoint(local)) {
            const auto& b=e.localBounds;
            const float x=local.x>=b.b.x?(b.t.x>=local.x?local.x:b.t.x):b.b.x;
            const float y=local.y>=b.b.y?(b.t.y>=local.y?local.y:b.t.y):b.b.y;
            const float z=local.z>=b.b.z?(b.t.z>=local.z?local.z:b.t.z):b.b.z;
            fraction=std::sqrt((local.x-x)*(local.x-x)+(local.z-z)*(local.z-z)+(local.y-y)*(local.y-y))/length;
            if(!std::isfinite(fraction))throw std::invalid_argument("WMO candidate distance overflow");
        }
        result.push_back({index,fraction});
    }
    if(!result.empty())std::qsort(result.data(),result.size(),sizeof(Candidate),[](const void* a,const void* b) {
        const auto x=static_cast<const Candidate*>(a)->fraction,y=static_cast<const Candidate*>(b)->fraction;
        return y>x?-1:y<x?1:0;
    });return result;
}
bool WmoHitFrame::Intersect(const C3Segment& segment,float& fraction,uint32_t flags,uint32_t ignore,Hit& output) const {
    if(!std::isfinite(fraction))throw std::invalid_argument("Nonfinite WMO query clip");
    const auto candidates=Candidates(segment,ignore);const auto mask=uint16_t(CMapObj::CreateWmoIgnoreFlags(flags));bool found=false;
    for(const auto& candidate:candidates) {
        if(candidate.fraction>fraction)continue;const auto& e=entries[candidate.entry];
        C3Segment local;local.b=segment.b*e.inverse;local.t=segment.t*e.inverse;
        if(!Finite(local.b)||!Finite(local.t))throw std::invalid_argument("WMO local query extent overflow");
        if(!Bounds(e.localBounds,local))continue;
        int32_t face=-1;bool hit=e.geometry && e.geometry->Intersect(local,fraction,mask,face);
        if(e.liquid && e.liquid->Intersect(local,fraction,flags)){hit=true;face=0;}
        if(hit){found=true;output={e.guid,e.groupKey,face};}
    }return found;
}
