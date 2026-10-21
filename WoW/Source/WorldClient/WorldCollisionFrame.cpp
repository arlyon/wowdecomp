#include "WorldCollisionFrame.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>
WorldCollisionFrame::WorldCollisionFrame(std::shared_ptr<const WmoHitFrame> w,std::shared_ptr<const TerrainHitFrame> t,std::shared_ptr<const WorldModelHitFrame> m)
    :wmo(std::move(w)),terrain(std::move(t)),models(std::move(m)) {
    if(!wmo || !terrain || !models)throw std::invalid_argument("Incomplete owned world collision producers");
}
std::shared_ptr<const WorldCollisionFrame> WorldCollisionFrame::Capture() {
    // Ordered owner-side capture; no callback or original-world access in queries.
    const auto wmo=WmoHitFrame::Capture();const auto terrain=TerrainHitFrame::Capture();const auto models=WorldModelHitFrame::Capture();
    return std::make_shared<const WorldCollisionFrame>(wmo,terrain,models);
}
std::vector<uint64_t> WorldCollisionFrame::CollisionGuids() const {
    auto result=wmo->CollisionGuids();const auto other=models->CollisionGuids();result.insert(result.end(),other.begin(),other.end());
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
bool WorldCollisionFrame::Intersect(CM2Scene& scratch,const C3Vector& start,const C3Vector& end,float clip,uint32_t flags,Hit& output) const {
    if(!std::isfinite(clip))throw std::invalid_argument("Nonfinite owned world collision clip");
    auto session=models->BeginQuery(flags);C3Segment segment;segment.b=start;segment.t=end;
    float best=clip;WmoHitFrame::Hit geometry;bool hit=wmo->Intersect(segment,best,flags,0x100,geometry);uint64_t guid=0;
    std::vector<uint64_t> scopes;
    for(const auto& candidate:wmo->Candidates(segment,0x100))scopes.push_back(WorldModelHitFrame::WmoKey(wmo->GroupKey(candidate.entry)));
    float value=best;const auto owner=models->Query(scratch,session,WorldModelHitFrame::Phase::Wmo,scopes,start,end,value);
    if(owner && value<best){best=value;guid=models->CollisionGuid(owner);hit=true;}
    // CMap::Intersect applies the retained geometric definition GUID AFTER the
    // WMO model result. A zero definition GUID does not overwrite a model GUID.
    if(hit && geometry.guid)guid=geometry.guid;
    scopes.clear();uint32_t previous=UINT32_MAX;
    for(const auto& cell:TerrainHitCells(start,end)) {
        if(cell.x<0 || cell.y<0 || cell.x>8192 || cell.y>8192)break;
        const uint32_t key=((uint32_t(cell.y)>>3)&1023)*1024+((uint32_t(cell.x)>>3)&1023);
        if(key==previous)continue;if(!models->HasScope(key))break;previous=key;scopes.push_back(key);
    }
    value=best;terrain->Intersect(start,end,value,flags&0x30100);
    models->Query(scratch,session,WorldModelHitFrame::Phase::Terrain,scopes,start,end,value);
    if(value<best){best=value;guid=0;hit=true;}
    if(!hit)return false;output.fraction=best;output.guid=guid;
    output.point=C3Vector((end.x-start.x)*best+start.x,(end.y-start.y)*best+start.y,(end.z-start.z)*best+start.z);return true;
}
