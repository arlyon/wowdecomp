#include "Engine/Source/Model2/M2HitSnapshot.h"
#include "Engine/Source/Services/Camera.h"
#include "WoW/Source/Ui/WorldModelPickFrame.h"
#include "WoW/Source/Ui/WorldInteractionFrame.h"
#include "WoW/Source/WorldClient/WorldDisplayEpoch.h"
#include "WoW/Source/WorldClient/WorldModelHitFrame.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Model2/M2Cache.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
namespace {
template<class T,size_t N> void Bind(M2Array<T>& target,T (&values)[N]){target.m_count=N;target.Set(values);}
struct Fixture {
    CM2Scene scene{nullptr};
    // Give the real legacy constructors defined initial flag bits (they preserve
    // some bits). References do not own the borrowed fixture arrays.
    alignas(CM2Shared) unsigned char sharedStorage[sizeof(CM2Shared)]{};
    alignas(CM2Model) unsigned char modelStorage[sizeof(CM2Model)]{};
    CM2Shared& shared=*::new(sharedStorage) CM2Shared(nullptr);
    CM2Model& model=*::new(modelStorage) CM2Model();
    M2Data data{};M2SkinProfile skin{};M2Sequence sequence[1]{};M2CompBone definitions[2]{};
    M2ModelBone pose;C44Matrix matrices[2];M2Vertex vertices[3]{};
    uint16_t mapping[3]{0,1,2},indices[3]{0,1,2},collisionIndices[3]{0,1,2};
    M2SkinSection section[1]{};M2Batch batch[1]{};M2Material material[1]{};uint32_t visible[1]{1};
    C3Vector collisionPositions[3]{{-1,-1,2},{1,-1,2},{0,1,2}};
    Fixture() {
        Bind(data.m_sequences,sequence);Bind(data.m_bones,definitions);Bind(data.m_vertices,vertices);Bind(data.m_materials,material);
        Bind(data.m_collisionPositions,collisionPositions);Bind(data.m_collisionIndices,collisionIndices);
        Bind(skin.m_vertices,mapping);Bind(skin.m_indices,indices);Bind(skin.m_skinSections,section);Bind(skin.m_batches,batch);
        for(unsigned i=0;i<3;++i){vertices[i].m_position=collisionPositions[i];vertices[i].m_normal=C3Vector(0,0,1);vertices[i].m_weights.b[0]=255;}
        section[0].m_vertexCount=3;section[0].m_indexCount=3;section[0].m_boneInfluences=1;
        sequence[0].m_bounds.m_extent.t=sequence[0].m_bounds.m_extent.b=C3Vector(0,0,0);
        sequence[0].m_bounds.m_radius=10;data.m_collisionBounds=sequence[0].m_bounds;
        shared.m_data=&data;shared.m_skinData=&skin;model.m_shared=&shared;model.m_bones=&pose;
        model.m_boneMatrices=matrices;model.m_skinSections=visible;model.m_alpha=1;model.m_fFlags=1;
        scene.m_frameStamp=model.m_frameStamp=7;pose.m_sequence.m_sequenceIndex=0;
        scene.m_lastHit={};scene.m_lastHitOwner=nullptr;
        model.m_matrixF4=C44Matrix();model.m_worldTransform=C44Matrix();scene.m_view=C44Matrix();
    }
    ~Fixture() {
        // Only source fixture lifecycle is synthetic. Never let production
        // destructors free borrowed stack arrays; all query algorithms are real.
        model.m_fFlags=0;model.m_bones=nullptr;model.m_boneMatrices=nullptr;model.m_skinSections=nullptr;
        model.m_attachList=model.m_attachParent=model.m_attachNext=nullptr;model.m_attachPrev=nullptr;
        // The real model destructor always visits shared sequence loads and
        // releases its reference. Keep that real owner alive until afterwards.
        shared.m_refCount=2;
        model.m_hitTestPrev=nullptr;model.m_hitTestNext=nullptr;shared.m_data=nullptr;shared.m_skinData=nullptr;scene.m_hitTestList=nullptr;
        model.~CM2Model();shared.~CM2Shared();
    }
    uint64_t Original(const C3Vector& start,const C3Vector& end,float& t,uint32_t mode=0) {
        scene.BeginHitTest();scene.m_hitTestList=&model;model.m_hitTestPrev=&scene.m_hitTestList;model.m_hitTestNext=nullptr;
        model.m_hitTestOwner=reinterpret_cast<void*>(uintptr_t(17));model.m_hitTestGroup=3;model.m_hitTestMode=mode;
        return reinterpret_cast<uintptr_t>(scene.EndHitTest(start,end,t,1));
    }
    void CompareWorld(const C3Vector& start,const C3Vector& end,float clip=1) {
        const auto snapshot=M2CaptureWorldCollisionSnapshot(model);const auto capturedView=scene.m_view;
        scene.BeginHitTest();scene.m_hitTestList=&model;model.m_hitTestPrev=&scene.m_hitTestList;model.m_hitTestNext=nullptr;
        model.m_hitTestOwner=reinterpret_cast<void*>(uintptr_t(17));model.m_hitTestGroup=3;model.m_hitTestMode=3;
        float a=clip,b=clip;const auto original=reinterpret_cast<uintptr_t>(scene.EndHitTestCollisionWorld(start,end,a));
        const auto* scratch=scene.m_hitVerts;const auto capacity=scene.m_hitVertCapacity;const auto flags=scene.m_flags;const auto last=scene.m_lastHitOwner;
        const auto owned=M2QueryWorldCollisionSnapshots({{snapshot,17,3}},capturedView,start,end,b);
        assert(original==owned && a==b);
        assert(scene.m_hitVerts==scratch && scene.m_hitVertCapacity==capacity && scene.m_flags==flags && scene.m_lastHitOwner==last && !scene.m_hitTestList);
    }
    void Compare(const C3Vector& start,const C3Vector& end,uint32_t mode=0) {
        const auto snapshot=M2CaptureHitSnapshot(scene,model,mode);
        float a=1,b=1;const auto original=Original(start,end,a,mode);
        const auto* scratch=scene.m_hitVerts;const auto capacity=scene.m_hitVertCapacity;
        const auto last=scene.m_lastHitOwner;const auto flags=scene.m_flags;
        const auto frozen=M2QueryHitSnapshots(scene,{{snapshot,17,3}},start,end,b);
        assert(original==frozen && (!original || std::abs(a-b)<1e-6f));
        assert(scene.m_hitVerts==scratch && scene.m_hitVertCapacity==capacity && scene.m_lastHitOwner==last && scene.m_flags==flags && !scene.m_hitTestList);
    }
};
}
int main() {
    std::shared_ptr<const M2WorldCollisionSnapshot> retainedWorld;
    {
        Fixture f;
        for(int x=-5;x<=5;++x)for(int y=-5;y<=5;++y)for(float clip:{.1f,.3f,1.f})
            f.CompareWorld(C3Vector(0,0,0),C3Vector(x*.3f,y*.3f,10),clip);
        f.model.m_worldTransform=C44Matrix(2);f.model.m_worldTransform.d2=3;f.scene.m_view=C44Matrix(2);
        f.model.m_matrixF4=f.model.m_worldTransform*f.scene.m_view;
        f.CompareWorld(C3Vector(0,0,0),C3Vector(0,0,10));
        // Physics uses stale matrixF4 and accepts attached stale models; regular
        // EndHitTest frameCheck has intentionally different eligibility.
        Fixture parent;f.model.m_attachParent=&parent.model;f.model.m_frameStamp=5;
        f.model.m_worldTransform=C44Matrix();f.model.m_matrixF4=C44Matrix();f.scene.m_view=C44Matrix();
        assert(!M2CaptureHitSnapshot(f.scene,f.model,3));
        f.CompareWorld(C3Vector(0,0,0),C3Vector(0,0,10));
        retainedWorld=M2CaptureWorldCollisionSnapshot(f.model);
        f.model.m_matrixF4.d0=100;f.CompareWorld(C3Vector(0,0,0),C3Vector(0,0,10));
        f.model.m_worldTransform.d0=200;f.collisionPositions[0].z=100;f.model.m_fFlags=0;
    }
    {
        C44Matrix view;float fraction=1;
        assert(M2QueryWorldCollisionSnapshots({{retainedWorld,29,0}},view,C3Vector(0,0,0),C3Vector(0,0,10),fraction)==29 && fraction==.2f);
        Fixture bad;bad.model.m_worldTransform=C44Matrix(0);bool rejected=false;
        try{M2CaptureWorldCollisionSnapshot(bad.model);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    std::shared_ptr<const WorldModelHitFrame> retainedPolicy;
    {
        Fixture near,far;far.model.m_worldTransform.d2=far.model.m_matrixF4.d2=3;
        std::vector<WorldModelHitFrame::Model> models;
        for(auto* f:{&near,&far}) {
            WorldModelHitFrame::Model m;m.loaded=true;m.visible=M2CaptureHitSnapshot(f->scene,f->model,1);m.collision=M2CaptureHitSnapshot(f->scene,f->model,3);
            m.worldCollision=m.worldCollisionAfterView=M2CaptureWorldCollisionSnapshot(f->model);models.push_back(m);
        }
        std::vector<WorldModelHitFrame::Record> records(5);
        records[0].flags=records[1].flags=0x80;records[0].model=records[1].model=0;records[1].collisionGuid=44;
        for(unsigned i:{2u,3u,4u})records[i].kind=WorldModelHitFrame::Kind::Entity;
        records[2].flags=records[4].flags=2;records[2].model=1;records[4].model=0;records[2].callbackAccepted=records[4].callbackAccepted=true;
        const uint64_t wmo=WorldModelHitFrame::WmoKey(1);
        std::unordered_map<uint64_t,WorldModelHitFrame::Scope> scopes{{wmo,{{0,1},{3,4}}},{7,{{0,1},{2}}}};
        retainedPolicy=std::make_shared<const WorldModelHitFrame>(C44Matrix(),C44Matrix(),true,models,records,scopes);
        for(uint32_t flags:{0x1000124u,0x1020124u,0x100151u}) {
            auto session=retainedPolicy->BeginQuery(flags);float t=1;const C3Vector a(0,0,0),b(0,0,10);
            const auto hit=retainedPolicy->Query(near.scene,session,WorldModelHitFrame::Phase::Wmo,{wmo},a,b,t);
            assert(hit==(flags==0x100151?2u:1u) && std::abs(t-.2f)<1e-6f);
            assert(retainedPolicy->CollisionGuid(hit)==(flags==0x100151?44u:0u));
            t=1;const auto terrain=retainedPolicy->Query(near.scene,session,WorldModelHitFrame::Phase::Terrain,{7},a,b,t);
            assert(terrain==(flags==0x100151?3u:0u)); // shared query-tag suppression, entity early return, duplicate-root owner update
            if(terrain)assert(std::abs(t-.5f)<1e-6f);
        }
        auto session=retainedPolicy->BeginQuery(0x1000124);float t=1;bool rejected=false;
        try{retainedPolicy->Query(near.scene,session,WorldModelHitFrame::Phase::Terrain,{7},C3Vector(),C3Vector(0,0,10),t);}catch(const std::logic_error&){rejected=true;}assert(rejected);
        rejected=false;try{retainedPolicy->BeginQuery(0);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    {
        CM2Scene scratch{nullptr};auto session=retainedPolicy->BeginQuery(0x100151);float t=1;
        assert(retainedPolicy->Query(scratch,session,WorldModelHitFrame::Phase::Wmo,{WorldModelHitFrame::WmoKey(1)},C3Vector(),C3Vector(0,0,10),t)==2);
    }
    {
        Fixture f;f.model.m_frameStamp=5;f.model.m_worldTransform.d0=100; // stale matrixF4 still has centre0
        WorldModelHitFrame::Model m;m.loaded=true;m.collision=M2CaptureHitSnapshot(f.scene,f.model,3);
        m.worldCollision=M2CaptureWorldCollisionSnapshot(f.model);const auto updated=f.model.m_worldTransform*f.scene.m_view;
        m.worldCollisionAfterView=M2CaptureWorldCollisionSnapshot(f.model,&updated);m.viewUpdatesWorldSphere=true;
        std::vector<WorldModelHitFrame::Record> records(2);for(auto& r:records){r.flags=0x80;r.model=0;}
        const uint64_t key=WorldModelHitFrame::WmoKey(7);
        WorldModelHitFrame frame(C44Matrix(),C44Matrix(),false,{m},records,{{key,{{0},{}}},{3,{{1},{}}}});
        auto session=frame.BeginQuery(0x100151);const C3Vector a(100,0,0),b(100,0,10);float t=1;
        assert(frame.Query(f.scene,session,WorldModelHitFrame::Phase::Wmo,{key},a,b,t)==1);
        t=1;assert(frame.Query(f.scene,session,WorldModelHitFrame::Phase::Terrain,{3},a,b,t)==2 && std::abs(t-.2f)<1e-6f);
        assert(f.model.m_matrixF4.d0==0); // owned query models the cache change without mutating the source
        t=1;assert(f.Original(a,b,t,3)==17 && f.model.m_matrixF4.d0==100);f.CompareWorld(a,b);
    }
    {
        Fixture near,far;far.matrices[0].d2=3;
        const GxWorld::WorldFrameLease identity{0,6,7};const auto graph=M2HitSceneSnapshot::Capture(near.scene,{{&near.model,17},{&far.model,19}});
        const auto actors=std::make_shared<const WorldModelPickFrame>(identity,C3Vector(),CRect{0,0,1,1},graph);
        const auto emptyModels=std::make_shared<const WorldModelHitFrame>(C44Matrix(),C44Matrix(),false,std::vector<WorldModelHitFrame::Model>{},std::vector<WorldModelHitFrame::Record>{},std::unordered_map<uint64_t,WorldModelHitFrame::Scope>{});
        const auto world=std::make_shared<const WorldCollisionFrame>(std::make_shared<const WmoHitFrame>(std::vector<WmoHitFrame::Entry>{}),TerrainHitFrame::Create({}),emptyModels);
        WorldInteractionFrame::Screen screen;screen.nearZ=1;screen.viewportNDC=screen.rect=CRect{0,0,1,1};
        C44Matrix transportInverse;transportInverse.d0=-100;
        const WorldInteractionFrame frame(actors,world,screen,{{55,transportInverse}},WorldDisplayEpoch(),17);auto state=frame.BeginInteraction();transportInverse.d0=-999;
        GxWorld::WorldFrame completed;completed.lease=identity;completed.description.scene=WorldDisplayEpoch();completed.description.width=320;completed.description.height=240;
        completed.description.view=completed.description.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};completed.description.projection[11]=1;completed.description.projection[14]=-1;completed.description.projection[15]=0;
        WorldInteractionFrame::Hit raw;raw.kind=3;raw.guid=55;raw.distance=7;raw.point=C3Vector(110,2,4);
        const auto local=frame.ResolveCollision(completed,raw,WorldInteractionFrame::CollisionObject::Transport);
        assert(local.kind==3 && local.guid==55 && local.point.x==10 && local.point.y==2 && local.distance==7);
        const auto ordinary=frame.ResolveCollision(completed,raw,WorldInteractionFrame::CollisionObject::Ordinary);assert(ordinary.point.x==110);
        const auto missing=frame.ResolveCollision(completed,raw,WorldInteractionFrame::CollisionObject::Missing);assert(missing.kind==0 && !missing.guid && missing.point.x==110);
        raw.guid=56;bool missingTransform=false;try{frame.ResolveCollision(completed,raw,WorldInteractionFrame::CollisionObject::Transport);}catch(const std::logic_error&){missingTransform=true;}assert(missingTransform);
        const C3Vector a(0,0,0),b(0,0,10);
        auto hit=frame.Pick(near.scene,completed,state,a,b,4,{{0,3,false},{1,9,false}});
        assert(hit.kind==2 && hit.guid==17 && std::abs(hit.distance-2)<1e-6f && hit.hasDepth && std::abs(hit.depth-2)<1e-6f);
        hit=frame.Pick(near.scene,completed,state,a,b,4,{{0,3,true},{1,0,false}});
        assert(hit.kind==2 && hit.guid==19 && std::abs(hit.distance-5)<1e-6f && state.ActorOrder()==std::vector<uint32_t>({1,0}));
        hit=frame.Pick(near.scene,completed,state,a,b,4,{{0,3,true}});assert(hit.guid==17); // low-priority only after normal miss
        near.model.m_alpha=0;near.matrices[0].d0=100;
        hit=frame.Pick(near.scene,completed,state,a,b,4,{{0,3,false}});assert(hit.guid==17); // paired original pose, no live lookup
        assert(frame.Pick(near.scene,completed,state,a,b,1,{}).kind==0);
        auto wrong=completed;++wrong.lease.generation;bool rejected=false;
        try{frame.Pick(near.scene,wrong,state,a,b,4,{});}catch(const std::logic_error&){rejected=true;}assert(rejected);
        rejected=false;try{frame.Pick(near.scene,completed,state,a,b,4,{{2,0,false}});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
        assert(frame.MatchesOwnerVersion(completed,WorldDisplayEpoch(),17));
        assert(!frame.MatchesOwnerVersion(completed,WorldDisplayEpoch(),18) && !frame.MatchesOwnerVersion(completed,WorldDisplayEpoch(),0));
        const auto epoch=WorldDisplayEpoch();InvalidateWorldDisplay();
        assert(WorldDisplayEpoch()==epoch+1 && !frame.MatchesOwnerVersion(completed,WorldDisplayEpoch(),17) && !frame.OwnerMatches(completed) && frame.Matches(completed));
    }
    const auto features=CM2Cache::s_processorFeatures;
    for(unsigned sse:{0u,4u}) {
        CM2Cache::s_processorFeatures=sse;
        Fixture f;
        for(int x=-5;x<=5;++x)for(int y=-5;y<=5;++y)f.Compare(C3Vector(0,0,0),C3Vector(x*.3f,y*.3f,10));
        f.Compare(C3Vector(0,0,0),C3Vector(0,0,10),3);
        f.section[0].m_boneInfluences=2;
        for(auto& vertex:f.vertices){vertex.m_weights.b[0]=128;vertex.m_weights.b[1]=127;vertex.m_indices.b[1]=1;}
        f.matrices[1].d2=2;
        f.shared.m_hitGeometrySnapshot.reset();
        f.Compare(C3Vector(0,0,0),C3Vector(0,0,10));
        f.material[0].m_blendMode=1;
        f.Compare(C3Vector(0,0,0),C3Vector(0,0,10),1);f.Compare(C3Vector(0,0,0),C3Vector(0,0,10),2);
        f.material[0].m_blendMode=0;
        for(auto& vertex:f.vertices){vertex.m_position.x+=3;vertex.m_normal=C3Vector(-3,0,0);}
        f.shared.m_hitGeometrySnapshot.reset();
        f.Compare(C3Vector(0,0,0),C3Vector(0,0,10)); // original expanded-normal second pass
        const auto frozen=M2CaptureHitSnapshot(f.scene,f.model);
        f.model.m_alpha=0;f.visible[0]=0;f.matrices[0].d0=f.matrices[1].d0=100;
        float t=1;assert(M2QueryHitSnapshots(f.scene,{{frozen,17,3}},C3Vector(0,0,0),C3Vector(0,0,10),t)==17);
        assert(t>0 && t<1);
    }
    CM2Cache::s_processorFeatures=features;
    for(bool expanded:{false,true}) {
        Fixture near,far;far.matrices[0].d2=3;
        if(expanded)for(auto* fixture:{&near,&far})for(auto& vertex:fixture->vertices) {
            vertex.m_position.x+=3;vertex.m_normal=C3Vector(-3,0,0);
        }
        const std::vector<M2HitSnapshotCandidate> candidates{
            {M2CaptureHitSnapshot(near.scene,near.model),17,0},{M2CaptureHitSnapshot(near.scene,far.model),19,9}};
        near.scene.BeginHitTest();near.scene.m_hitTestList=&near.model;
        near.model.m_hitTestPrev=&near.scene.m_hitTestList;near.model.m_hitTestNext=&far.model;
        far.model.m_hitTestPrev=&near.model.m_hitTestNext;far.model.m_hitTestNext=nullptr;
        near.model.m_hitTestOwner=reinterpret_cast<void*>(uintptr_t(17));far.model.m_hitTestOwner=reinterpret_cast<void*>(uintptr_t(19));
        near.model.m_hitTestGroup=0;far.model.m_hitTestGroup=9;near.model.m_hitTestMode=far.model.m_hitTestMode=0;
        float a=1,b=1;const C3Vector start(0,0,0),end(0,0,10);
        const auto original=reinterpret_cast<uintptr_t>(near.scene.EndHitTest(start,end,a,1));
        const auto frozen=M2QueryHitSnapshots(near.scene,candidates,start,end,b);
        assert(original==(expanded?19u:17u) && frozen==original && std::abs(a-b)<1e-6f);
    }
    std::shared_ptr<const M2HitSceneSnapshot> retainedGraph;
    {
        Fixture parent,child;parent.model.m_attachList=&child.model;child.model.m_attachParent=&parent.model;
        parent.matrices[0].d0=20;parent.model.m_matrixF4.d0=20;
        const auto graph=M2HitSceneSnapshot::Capture(parent.scene,{{&parent.model,17},{&child.model,19},{&parent.model,23}});
        assert(graph->RootCount()==3 && graph->RootOwner(0)==17 && graph->RootOwner(1)==19);
        const auto pick=[&](const std::vector<M2HitRootSelection>& selected) {
            float t=1;return graph->Query(parent.scene,selected,C3Vector(0,0,0),C3Vector(0,0,10),t);
        };
        assert(pick({{0,3}})==17); // an unselected explicit child root must still inherit its selected parent
        assert(pick({{1,9}})==19);
        assert(pick({{0,3},{1,9}})==19 && pick({{1,9},{0,3}})==19);
        assert(pick({{0,3},{2,7}})==23); // repeated root updates ownership without reinserting descendants
        child.matrices[0].d0=100;child.model.m_fFlags=0;
        assert(pick({{0,3}})==17); // no live eligibility/pose lookup during a query
        bool rejected=false;try{pick({{3,0}});}catch(const std::out_of_range&){rejected=true;}assert(rejected);
        retainedGraph=graph;
    }
    {
        CM2Scene scratch{nullptr};float t=1;
        assert(retainedGraph->Query(scratch,{{0,3}},C3Vector(0,0,0),C3Vector(0,0,10),t)==17);
    }
    {
        Fixture parent,child;parent.model.m_attachList=&child.model;child.model.m_attachParent=&parent.model;
        child.model.m_attachList=&parent.model;parent.model.m_attachParent=&child.model;
        bool rejected=false;try{M2HitSceneSnapshot::Capture(parent.scene,{{&parent.model,17}});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    {
        const GxWorld::WorldFrameLease identity{1,11,13};const C3Vector camera(10,20,30);
        const auto frame=std::make_shared<const WorldModelPickFrame>(identity,camera,CRect{0,0,1,1},retainedGraph);
        GxWorld::WorldFrame completed;completed.lease=identity;completed.description.scene=WorldDisplayEpoch();completed.description.width=320;completed.description.height=240;
        completed.description.view=completed.description.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        C3Vector start,end;assert(frame->LineSegment(completed,.5f,.5f,start,end));
        assert(start.x==10 && start.y==20 && end.z>start.z);
        CM2Scene scratch{nullptr};float distance=-1;
        assert(frame->FindClosest(scratch,completed,{{0,3}},camera,camera+C3Vector(0,0,10),distance)==17 && std::abs(distance-2)<1e-6f);
        auto wrong=completed;++wrong.lease.epoch;
        assert(!frame->LineSegment(wrong,.5f,.5f,start,end));
        bool rejected=false;try{frame->FindClosest(scratch,wrong,{{0,3}},camera,camera+C3Vector(0,0,10),distance);}
        catch(const std::logic_error&){rejected=true;}assert(rejected);
    }
    {
        // Goldshire has posed, loaded M2s with no render batches/sections.
        // The original geometry query visits no triangles, so no section pose
        // is needed. Collision geometry remains independently queryable.
        Fixture empty;
        empty.skin.m_batches.m_count=0;empty.skin.m_skinSections.m_count=0;
        empty.model.m_skinSections=nullptr;
        for(uint32_t mode:{0u,1u,2u}) {
            assert(!M2CaptureHitSnapshot(empty.scene,empty.model,mode));
            empty.Compare(C3Vector(0,0,0),C3Vector(0,0,10),mode);
        }
        empty.Compare(C3Vector(0,0,0),C3Vector(0,0,10),3);
        empty.model.m_boneMatrices=nullptr;
        assert(!M2CaptureHitSnapshot(empty.scene,empty.model,1));
        assert(M2CaptureHitSnapshot(empty.scene,empty.model,3));
        Fixture parent;
        parent.model.m_attachList=&empty.model;empty.model.m_attachParent=&parent.model;
        const auto graph=M2HitSceneSnapshot::Capture(parent.scene,{{&parent.model,17}},1);
        float fraction=1;
        assert(graph->Query(parent.scene,{{0,3}},C3Vector(0,0,0),C3Vector(0,0,10),fraction)==17 && std::abs(fraction-.2f)<1e-6f);
    }
    {
        Fixture malformed;malformed.model.m_skinSections=nullptr;
        bool rejected=false;try{M2CaptureHitSnapshot(malformed.scene,malformed.model);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    std::shared_ptr<const M2HitSnapshot> retained;
    {Fixture source;retained=M2CaptureHitSnapshot(source.scene,source.model);}
    CM2Scene independent{nullptr};float t=1;
    assert(M2QueryHitSnapshots(independent,{{retained,23,0}},C3Vector(0,0,0),C3Vector(0,0,10),t)==23 && std::abs(t-.2f)<1e-6f);
    {
        Fixture malformed;malformed.indices[2]=3;
        bool rejected=false;try{M2CaptureHitSnapshot(malformed.scene,malformed.model);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    {
        Fixture malformed;malformed.vertices[0].m_indices.b[0]=2;
        bool rejected=false;try{M2CaptureHitSnapshot(malformed.scene,malformed.model);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    {
        Fixture malformed;malformed.matrices[0].a0=std::numeric_limits<float>::infinity();
        bool rejected=false;try{M2CaptureHitSnapshot(malformed.scene,malformed.model);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    t=1;bool rejected=false;
    try{M2QueryHitSnapshots(independent,{{retained,23,0}},C3Vector(std::numeric_limits<float>::infinity(),0,0),C3Vector(0,0,10),t);}
    catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    {
        CGxDeviceVulkan device; // CPU logical state only: no DeviceCreate/SDL/Vulkan instance
        g_theGxDevicePtr=&device;
        const C44Matrix view,projection;
        device.m_xforms[GxXform_View].m_level=0;device.m_xforms[GxXform_View].m_mtx[0]=view;device.m_projection=projection;
        C3Vector originalStart,originalEnd,frozenStart,frozenEnd,currentStart,currentEnd;
        CameraGetLineSegment(.25f,.75f,&originalStart,&originalEnd);
        device.m_xforms[GxXform_View].m_mtx[0].Translate(C3Vector(10,20,30));device.m_projection.Scale(2.f);
        CameraGetLineSegmentFromTransforms(.25f,.75f,view,projection,&frozenStart,&frozenEnd);
        CameraGetLineSegment(.25f,.75f,&currentStart,&currentEnd);
        const auto same=[](const C3Vector& a,const C3Vector& b){return a.x==b.x && a.y==b.y && a.z==b.z;};
        assert(same(originalStart,frozenStart) && same(originalEnd,frozenEnd));
        assert(!same(originalStart,currentStart) || !same(originalEnd,currentEnd));
        g_theGxDevicePtr=nullptr;
    }
    std::puts("PASS M2 hit snapshot: original single/blended/SSE/collision/material/two-pass query parity; immutable pose/topology; source destruction; scratch/last-hit preservation; invalid data rejection; frozen camera rays ignore current Gx transforms");
}
