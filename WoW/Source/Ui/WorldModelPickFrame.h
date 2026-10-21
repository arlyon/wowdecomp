#ifndef WOW_SOURCE_UI_WORLDMODELPICKFRAME_H
#define WOW_SOURCE_UI_WORLDMODELPICKFRAME_H
#include "Engine/Source/Gx/CompletedWorld.h"
#include "Engine/Source/Model2/M2HitSnapshot.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/Tempest/C4Vector.h"
class WorldModelPickFrame {
    GxWorld::WorldFrameLease identity;
    C3Vector camera;
    CRect viewport;
    std::shared_ptr<const M2HitSceneSnapshot> models;
public:
    WorldModelPickFrame(GxWorld::WorldFrameLease id,C3Vector position,CRect rect,std::shared_ptr<const M2HitSceneSnapshot> geometry);
    bool Matches(const GxWorld::WorldFrame& frame) const{return frame.lease==identity;}
    size_t RootCount() const{return models->RootCount();}
    uint64_t RootOwner(uint32_t root) const{return models->RootOwner(root);}
    bool LineSegment(const GxWorld::WorldFrame&,float,float,C3Vector&,C3Vector&) const;
    C4Vector Project(const GxWorld::WorldFrame&,const C3Vector&) const;
    uint64_t FindClosest(CM2Scene&,const GxWorld::WorldFrame&,const std::vector<M2HitRootSelection>&,const C3Vector&,const C3Vector&,float&) const;
};
#endif
