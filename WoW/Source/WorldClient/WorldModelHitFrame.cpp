#include "WorldModelHitFrame.h"
#include "Map.h"
#include "MapArea.h"
#include "MapChunk.h"
#include "MapObj.h"
#include "MapObjDef.h"
#include "MapObjGroup.h"
#include "MapDoodadDef.h"
#include "MapEntity.h"
#include "World.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
namespace {
void Matrix(const C44Matrix& m) {
    for(float v:{m.a0,m.a1,m.a2,m.a3,m.b0,m.b1,m.b2,m.b3,m.c0,m.c1,m.c2,m.c3,m.d0,m.d1,m.d2,m.d3})
        if(!std::isfinite(v))throw std::invalid_argument("Nonfinite original world model query transform");
}
uint32_t Index(size_t count){if(count>=UINT32_MAX)throw std::overflow_error("World model frame index exhausted");return uint32_t(count);}
}
WorldModelHitFrame::WorldModelHitFrame(C44Matrix v,C44Matrix t,bool cb,std::vector<Model> m,std::vector<Record> r,std::unordered_map<uint64_t,Scope> s)
    :view(v),transport(t),callbacks(cb),models(std::move(m)),records(std::move(r)),scopes(std::move(s)) {
    Matrix(view);Matrix(transport);Index(models.size());Index(records.size());
    for(const auto& record:records)if(record.model!=UINT32_MAX && record.model>=models.size())throw std::invalid_argument("World model record exceeds owned models");
    for(const auto& pair:scopes) {
        for(auto id:pair.second.doodads)if(id>=records.size() || records[id].kind!=Kind::Doodad)throw std::invalid_argument("Invalid original doodad scope record");
        for(auto id:pair.second.entities)if(id>=records.size() || records[id].kind!=Kind::Entity)throw std::invalid_argument("Invalid original entity scope record");
    }
}
std::shared_ptr<const WorldModelHitFrame> WorldModelHitFrame::Capture() {
    auto* scene=World::s_m2Scene;if(!scene)throw std::invalid_argument("Missing original world model scene");
    std::vector<Model> models;std::vector<Record> records;std::unordered_map<uint64_t,Scope> scopes;
    std::unordered_map<const CM2Model*,uint32_t> modelIds;std::unordered_map<const CMapBaseObj*,uint32_t> recordIds;
    std::vector<uint8_t> visibleCaptured;
    const auto internModel=[&](const CM2Model* source,bool visible) {
        if(!source)return UINT32_MAX;
        if((source->m_fFlags&1) && source->m_scene!=scene)throw std::invalid_argument("Original world collision model belongs to another scene");
        auto found=modelIds.find(source);uint32_t id;
        if(found==modelIds.end()) {
            id=Index(models.size());modelIds.emplace(source,id);Model model;model.loaded=(source->m_fFlags&1)!=0;
            model.collision=M2CaptureHitSnapshot(*scene,*source,3);model.worldCollision=M2CaptureWorldCollisionSnapshot(*source);
            model.viewUpdatesWorldSphere=model.loaded && source->m_frameStamp!=UINT32_MAX && source->m_frameStamp!=scene->m_frameStamp && !source->m_attachParent;
            if(model.viewUpdatesWorldSphere) {const auto updated=source->m_worldTransform*scene->m_view;model.worldCollisionAfterView=M2CaptureWorldCollisionSnapshot(*source,&updated);}
            else model.worldCollisionAfterView=model.worldCollision;
            models.push_back(std::move(model));visibleCaptured.push_back(0);
        }else id=found->second;
        if(visible && !visibleCaptured[id]){models[id].visible=M2CaptureHitSnapshot(*scene,*source,1);visibleCaptured[id]=1;}
        return id;
    };
    const auto internDoodad=[&](CMapDoodadDef* source) {
        if(!source)throw std::invalid_argument("Missing original doodad scope owner");const auto found=recordIds.find(source);if(found!=recordIds.end())return found->second;
        const auto id=Index(records.size());Record r;r.kind=Kind::Doodad;r.flags=uint32_t(source->m_flags);r.culled=source->m_culled;r.collisionGuid=source->m_collisionGuid.m_guid;
        if(!(r.flags&0x100) && (r.flags&0x80))r.model=internModel(source->m_model,!r.culled && !r.collisionGuid);
        recordIds.emplace(source,id);records.push_back(std::move(r));return id;
    };
    const auto internEntity=[&](CMapEntity* source) {
        if(!source)throw std::invalid_argument("Missing original entity scope owner");const auto found=recordIds.find(source);if(found!=recordIds.end())return found->second;
        const auto id=Index(records.size());Record r;r.kind=Kind::Entity;r.flags=source->m_staticFlags;r.culled=source->m_culled;
        if(World::s_objectCollisionCallback && (r.flags&2) && !(r.flags&4)) {
            World::ObjectCollisionData data;WOWGUID guid{source->m_funcParam64};
            r.callbackAccepted=World::s_objectCollisionCallback(&guid,source->m_funcParam32,&data,0x100151)!=0;
            if(r.callbackAccepted) {
                r.callbackGuid=guid.m_guid;r.callbackBounds=data.m_bounds;r.callbackScale=data.m_scale;r.callbackMatrix=data.m_mat;
                r.model=internModel(data.m_model,false);
            }
        }
        recordIds.emplace(source,id);records.push_back(std::move(r));return id;
    };
    const auto captureScope=[&](uint64_t key,auto& doodads,auto& entities) {
        Scope scope;std::unordered_set<const CMapBaseObjLink*> seen;
        for(auto* link=doodads.Head();link;link=doodads.Next(link)) {
            if(!seen.insert(link).second)throw std::invalid_argument("Cyclic original doodad scope");
            scope.doodads.push_back(internDoodad(static_cast<CMapDoodadDef*>(link->m_owner)));
        }
        seen.clear();
        if(World::s_objectCollisionCallback)for(auto* link=entities.Head();link;link=entities.Next(link)) {
            if(!seen.insert(link).second)throw std::invalid_argument("Cyclic original entity scope");
            auto* source=static_cast<CMapEntity*>(link->m_owner);scope.entities.push_back(internEntity(source));
            if(!(source->m_staticFlags&2))break; // original return, not continue
        }
        if(!scopes.emplace(key,std::move(scope)).second)throw std::invalid_argument("Duplicate original world model scope identity");
    };
    for(auto* def=CMap::mapObjDefHash.m_fulllist.Head();def;def=CMap::mapObjDefHash.m_fulllist.Next(def)) {
        auto* owner=def->m_owner;if(!owner || !owner->m_isGroupLoaded)continue;
        const auto count=def->m_defGroups.m_count==UINT32_MAX?def->m_defGroups.m_dynamic.m_count:def->m_defGroups.m_count;
        if(count>std::size(owner->m_mapObjGroupArray))throw std::invalid_argument("Original world model group allocation exceeded");
        for(uint32_t i=0;i<count;++i) {
            auto* loaded=owner->m_mapObjGroupArray[i];if(!loaded || !(loaded->m_unkLoadedFlag&1))continue;
            auto* group=def->m_defGroups[i];if(!group || group->m_groupNum!=i)throw std::invalid_argument("Invalid original world model group identity");
            captureScope(WmoKey(uint32_t(group->m_objectIndex)),group->m_doodadDefLinkList,group->m_entityLinkList);
        }
    }
    for(uint32_t area=0;area<4096;++area) {
        auto* loaded=CMap::m_areaTable[area];if(!loaded || loaded->m_asyncObject)continue;
        for(uint32_t i=0;i<256;++i) {
            auto* chunk=loaded->m_mapChunks[i];if(!chunk || !chunk->m_header || !chunk->m_height)continue;
            const uint32_t x=(area%64)*16+i%16,y=(area/64)*16+i/16;
            captureScope(uint64_t(y)*1024+x,chunk->m_doodadDefLinkList,chunk->m_entityLinkList);
        }
    }
    return std::make_shared<const WorldModelHitFrame>(scene->m_view,CWorldScene::camTransportView,World::s_objectCollisionCallback!=nullptr,std::move(models),std::move(records),std::move(scopes));
}
WorldModelHitFrame::Session WorldModelHitFrame::BeginQuery(uint32_t flags) const {
    if(flags!=0x1000124 && flags!=0x1020124 && flags!=0x100151)throw std::invalid_argument("World model frame cannot certify another collision policy");
    return Session(this,records.size(),models.size(),flags==0x100151);
}
uint64_t WorldModelHitFrame::Query(CM2Scene& scratch,Session& session,Phase phase,const std::vector<uint64_t>& keys,const C3Vector& start,const C3Vector& end,float& fraction) const {
    if(session.owner!=this)throw std::invalid_argument("World model query session belongs to another generation");
    if(phase==Phase::Wmo) {if(session.wmoDone)throw std::logic_error("Repeated WMO model phase");session.wmoDone=true;}
    else {if(!session.wmoDone || session.terrainDone)throw std::logic_error("Invalid terrain model phase order");session.terrainDone=true;}
    std::vector<uint8_t> included(models.size());std::vector<uint32_t> order;std::vector<uint64_t> owners(models.size());
    const auto insert=[&](uint32_t record) {
        const auto model=records[record].model;if(!models[model].loaded)return;
        if(!included[model]){included[model]=1;order.push_back(model);}owners[model]=uint64_t(record)+1;
    };
    for(auto key:keys) {
        const auto found=scopes.find(key);if(found==scopes.end())throw std::invalid_argument("World model query scope is not in captured generation");const auto& scope=found->second;
        for(auto id:scope.doodads) {
            const auto& r=records[id];if((!session.secondary && r.culled) || (r.flags&0x100) || !(r.flags&0x80) || session.seen[id] || r.model==UINT32_MAX)continue;
            session.seen[id]=1;if(!session.secondary && r.collisionGuid)continue;insert(id);
        }
        if(session.secondary && callbacks)for(auto id:scope.entities) {
            const auto& r=records[id];if(!(r.flags&2))break;
            if(session.seen[id] || (r.flags&4) || !r.callbackAccepted || r.model==UINT32_MAX)continue;
            session.seen[id]=1;insert(id);
        }
    }
    if(session.secondary && phase==Phase::Terrain) {
        std::vector<M2WorldCollisionCandidate> candidates;
        for(auto it=order.rbegin();it!=order.rend();++it){const auto id=*it;candidates.push_back({session.worldUpdated[id]?models[id].worldCollisionAfterView:models[id].worldCollision,owners[id],0});}
        return M2QueryWorldCollisionSnapshots(candidates,view,start,end,fraction);
    }
    std::vector<M2HitSnapshotCandidate> candidates;
    for(auto it=order.rbegin();it!=order.rend();++it) {
        const auto id=*it;candidates.push_back({session.secondary?models[id].collision:models[id].visible,owners[id],0});
        if(session.secondary && models[id].viewUpdatesWorldSphere)session.worldUpdated[id]=1;
    }
    const auto a=start*transport,b=end*transport;return M2QueryHitSnapshots(scratch,candidates,a,b,fraction,false);
}
uint64_t WorldModelHitFrame::CollisionGuid(uint64_t owner) const {
    if(!owner || owner>records.size())throw std::out_of_range("World collision owner is not captured");const auto& r=records[size_t(owner-1)];
    return r.kind==Kind::Doodad?r.collisionGuid:0;
}
