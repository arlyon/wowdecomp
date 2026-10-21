#ifndef ENGINE_SOURCE_MODEL2_M2HITSNAPSHOT_H
#define ENGINE_SOURCE_MODEL2_M2HITSNAPSHOT_H
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
class CM2Model;
class CM2Scene;
class M2HitGeometrySource;
class M2HitSnapshot;
class M2WorldCollisionSnapshot;
struct M2WorldCollisionCandidate {
    std::shared_ptr<const M2WorldCollisionSnapshot> pose;
    uint64_t owner=0;
    uint32_t priority=0;
};
// EndHitTestCollisionWorld has different frame/bounds/space semantics from
// EndHitTest, even when both use collision triangles.
class M2WorldCollisionSnapshot {
    std::shared_ptr<const M2HitGeometrySource> geometry;
    C44Matrix inverse;
    C3Vector centre;
    float radiusSquared=0;
    M2WorldCollisionSnapshot()=default;
    friend std::shared_ptr<const M2WorldCollisionSnapshot> M2CaptureWorldCollisionSnapshot(const CM2Model&,const C44Matrix*);
    friend uint64_t M2QueryWorldCollisionSnapshots(const std::vector<M2WorldCollisionCandidate>&,const C44Matrix&,const C3Vector&,const C3Vector&,float&);
};
// Optional capture input models the original view hit test's stale-root
// matrixF4 update before a later physics phase. Never retained or written.
std::shared_ptr<const M2WorldCollisionSnapshot> M2CaptureWorldCollisionSnapshot(const CM2Model&,const C44Matrix* sphereTransform=nullptr);
uint64_t M2QueryWorldCollisionSnapshots(const std::vector<M2WorldCollisionCandidate>&,const C44Matrix& capturedView,
    const C3Vector& start,const C3Vector& end,float& t);
struct M2HitSnapshotCandidate {
    std::shared_ptr<const M2HitSnapshot> pose;
    uint64_t owner=0;
    uint32_t priority=0;
};
// Original hit geometry and evaluated pose, not a live model lease. Topology is
// owned once per shared asset; per-generation data has no callback or actor ptr.
class M2HitSnapshot {
    std::shared_ptr<const M2HitGeometrySource> geometry;
    std::vector<C44Matrix> bones;
    std::vector<uint32_t> sections;
    C44Matrix collisionTransform;
    C3Vector centre;
    float radiusSquared=0;
    bool collision=false,sse=false;
    M2HitSnapshot()=default;
    friend std::shared_ptr<const M2HitSnapshot> M2CaptureHitSnapshot(CM2Scene&,const CM2Model&,uint32_t);
    friend uint64_t M2QueryHitSnapshots(CM2Scene&,const std::vector<M2HitSnapshotCandidate>&,const C3Vector&,const C3Vector&,float&,bool);
};
std::shared_ptr<const M2HitSnapshot> M2CaptureHitSnapshot(CM2Scene&,const CM2Model&,uint32_t mode=0);
// The scene is used ONLY for its existing triangle routine and temporarily
// replaced scratch storage. Its model/hit lists, current pose and last hit are
// neither consumed nor modified. Call on the scene owner thread.
uint64_t M2QueryHitSnapshots(CM2Scene&,const std::vector<M2HitSnapshotCandidate>&,
    const C3Vector& start,const C3Vector& end,float& t,bool twoPass=true);
// Source pointers exist only during owner-side capture. Queries select roots
// by index and resolve inherited attachment ownership entirely from owned data.
struct M2HitSnapshotRoot {const CM2Model* model;uint64_t owner;};
struct M2HitRootSelection {uint32_t root,priority;};
class M2HitSceneSnapshot {
    struct Node {
        std::shared_ptr<const M2HitSnapshot> pose;
        std::vector<uint32_t> children;
        uint32_t parent=UINT32_MAX;
        bool loaded=false;
    };
    struct Root {uint32_t node;uint64_t owner;};
    std::vector<Node> nodes;
    std::vector<Root> roots;
    M2HitSceneSnapshot()=default;
public:
    static std::shared_ptr<const M2HitSceneSnapshot> Capture(CM2Scene&,const std::vector<M2HitSnapshotRoot>&,uint32_t mode=0);
    std::size_t RootCount() const {return roots.size();}
    uint64_t RootOwner(uint32_t root) const;
    uint64_t Query(CM2Scene&,const std::vector<M2HitRootSelection>&,const C3Vector&,const C3Vector&,float& t,bool twoPass=true) const;
};
#endif
