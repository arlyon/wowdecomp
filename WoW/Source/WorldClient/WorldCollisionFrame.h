#ifndef WOW_SOURCE_WORLDCLIENT_WORLDCOLLISIONFRAME_H
#define WOW_SOURCE_WORLDCLIENT_WORLDCOLLISIONFRAME_H
#include "WmoHitFrame.h"
#include "TerrainHitSnapshot.h"
#include "WorldModelHitFrame.h"
// Complete owned collision producers for the three CGWorldFrame UI policies.
// Camera/publication/scene identity is bound by the enclosing interaction frame.
class WorldCollisionFrame {
    std::shared_ptr<const WmoHitFrame> wmo;
    std::shared_ptr<const TerrainHitFrame> terrain;
    std::shared_ptr<const WorldModelHitFrame> models;
public:
    struct Hit {float fraction=1;uint64_t guid=0;C3Vector point;};
    WorldCollisionFrame(std::shared_ptr<const WmoHitFrame>,std::shared_ptr<const TerrainHitFrame>,std::shared_ptr<const WorldModelHitFrame>);
    static std::shared_ptr<const WorldCollisionFrame> Capture();
    std::vector<uint64_t> CollisionGuids() const;
    bool Intersect(CM2Scene& scratch,const C3Vector&,const C3Vector&,float clip,uint32_t flags,Hit&) const;
};
#endif
