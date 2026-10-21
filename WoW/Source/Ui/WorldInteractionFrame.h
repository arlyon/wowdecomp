#ifndef WOW_SOURCE_UI_WORLDINTERACTIONFRAME_H
#define WOW_SOURCE_UI_WORLDINTERACTIONFRAME_H
#include "WorldModelPickFrame.h"
#include "WoW/Source/WorldClient/WorldCollisionFrame.h"
#include <unordered_map>
class CGWorldFrame;
struct HITTESTRESULT;
class WorldInteractionFrame {
public:
    using Frame=GxWorld::WorldFrame;
    struct Screen{float nearZ=0,ddcWidth=1,ddcHeight=1;CRect viewportNDC,rect;};
    struct ActorPolicy{uint32_t root,priority;bool lowPriority=false;};
    enum class CollisionObject{Missing,Ordinary,Transport};
    class State{
        friend class WorldInteractionFrame;
        const WorldInteractionFrame* owner;
        std::vector<uint32_t> order;
        explicit State(const WorldInteractionFrame*);
    public:
        const std::vector<uint32_t>& ActorOrder() const{return order;}
    };
    struct Hit{int32_t kind=0;float distance=0,depth=0;bool hasDepth=false;C3Vector point;uint64_t guid=0;};
    WorldInteractionFrame(std::shared_ptr<const WorldModelPickFrame>,std::shared_ptr<const WorldCollisionFrame>,Screen,
        std::unordered_map<uint64_t,C44Matrix> transports={},uint64_t lifecycle=0,uint64_t device=0);
    State BeginInteraction() const{return State(this);}
    bool Matches(const Frame& frame) const{return actors->Matches(frame);}
    bool MatchesOwnerVersion(const Frame& frame,uint64_t epoch,uint64_t device) const{
        return lifecycleEpoch==epoch && frame.description.scene==epoch && presentationVersion==device && Matches(frame);
    }
    bool OwnerMatches(const Frame&) const;
    uint64_t ActorGuid(uint32_t root) const{return actors->RootOwner(root);}
    size_t ActorCount() const{return actors->RootCount();}
    bool LineSegment(const Frame& frame,float x,float y,C3Vector& a,C3Vector& b) const{return actors->LineSegment(frame,x,y,a,b);}
    bool ScreenCoordinates(const Frame&,const C3Vector&,C3Vector&,uint32_t*) const;
    Hit Pick(CM2Scene&,const Frame&,State&,const C3Vector&,const C3Vector&,uint32_t,const std::vector<ActorPolicy>&) const;
    std::vector<ActorPolicy> ActorPolicies(CGWorldFrame&,const Frame&,const State&,uint32_t) const;
    Hit ResolveCollision(const Frame&,Hit,CollisionObject) const;
    Hit PickOnOwner(CGWorldFrame&,const Frame&,State&,const C3Vector&,const C3Vector&,uint32_t) const;
    int32_t HitTestPointOnOwner(CGWorldFrame&,const Frame&,State&,float,float,int32_t,HITTESTRESULT*) const;
private:
    std::shared_ptr<const WorldModelPickFrame> actors;
    std::shared_ptr<const WorldCollisionFrame> world;
    Screen screen;
    uint64_t lifecycleEpoch=0,presentationVersion=0;
    std::unordered_map<uint64_t,C44Matrix> transportInverses;
    bool Depth(const Frame&,const C3Vector&,float&) const;
};
#endif
