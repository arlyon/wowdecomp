#ifndef WOW_SOURCE_WORLDCLIENT_WORLDMODELHITFRAME_H
#define WOW_SOURCE_WORLDCLIENT_WORLDMODELHITFRAME_H
#include "Engine/Source/Model2/M2HitSnapshot.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include <memory>
#include <unordered_map>
#include <vector>
// The three original CGWorldFrame::HitTest world policies, not arbitrary physics
// flags. Source callbacks run only in Capture, on the scene owner.
class WorldModelHitFrame {
public:
    enum class Kind {Doodad,Entity};
    enum class Phase {Wmo,Terrain};
    struct Model {
        bool loaded=false;
        std::shared_ptr<const M2HitSnapshot> visible,collision;
        std::shared_ptr<const M2WorldCollisionSnapshot> worldCollision,worldCollisionAfterView;
        bool viewUpdatesWorldSphere=false;
    };
    struct Record {
        Kind kind=Kind::Doodad;
        uint32_t flags=0,model=UINT32_MAX;
        bool culled=false,callbackAccepted=false;
        uint64_t collisionGuid=0,callbackGuid=0;
        CAaBox callbackBounds;
        float callbackScale=0;
        C44Matrix callbackMatrix;
    };
    struct Scope {std::vector<uint32_t> doodads,entities;};
    class Session {
        friend class WorldModelHitFrame;
        const WorldModelHitFrame* owner;
        std::vector<uint8_t> seen,worldUpdated;
        bool secondary,wmoDone=false,terrainDone=false;
        Session(const WorldModelHitFrame* f,size_t count,size_t modelCount,bool physics):owner(f),seen(count),worldUpdated(modelCount),secondary(physics){}
    };
    static uint64_t WmoKey(uint32_t group){return (uint64_t(1)<<63)|group;}
    static std::shared_ptr<const WorldModelHitFrame> Capture();
    WorldModelHitFrame(C44Matrix view,C44Matrix transport,bool callbacks,std::vector<Model>,std::vector<Record>,std::unordered_map<uint64_t,Scope>);
    Session BeginQuery(uint32_t flags) const;
    uint64_t Query(CM2Scene& scratch,Session&,Phase,const std::vector<uint64_t>& scopes,const C3Vector&,const C3Vector&,float&) const;
    bool HasScope(uint64_t key) const{return scopes.count(key)!=0;}
    uint64_t CollisionGuid(uint64_t owner) const;
    std::vector<uint64_t> CollisionGuids() const {std::vector<uint64_t> result;for(const auto& r:records)if(r.kind==Kind::Doodad && r.collisionGuid)result.push_back(r.collisionGuid);return result;}
private:
    C44Matrix view,transport;
    bool callbacks;
    std::vector<Model> models;
    std::vector<Record> records;
    std::unordered_map<uint64_t,Scope> scopes;
};
#endif
