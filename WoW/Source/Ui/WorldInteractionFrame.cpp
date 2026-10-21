#include "WorldInteractionFrame.h"
#include <cmath>
#include <numeric>
#include <stdexcept>
WorldInteractionFrame::State::State(const WorldInteractionFrame* frame):owner(frame),order(frame->ActorCount()) {
    std::iota(order.begin(),order.end(),0u);
}
WorldInteractionFrame::WorldInteractionFrame(std::shared_ptr<const WorldModelPickFrame> a,std::shared_ptr<const WorldCollisionFrame> w,Screen s,std::unordered_map<uint64_t,C44Matrix> inverses,uint64_t epoch,uint64_t device)
    :actors(std::move(a)),world(std::move(w)),screen(s),lifecycleEpoch(epoch),presentationVersion(device),transportInverses(std::move(inverses)) {
    if(!actors || !world || actors->RootCount()>UINT32_MAX)throw std::invalid_argument("Incomplete displayed world interaction frame");
    for(float v:{s.nearZ,s.ddcWidth,s.ddcHeight,s.viewportNDC.minX,s.viewportNDC.minY,s.viewportNDC.maxX,s.viewportNDC.maxY,s.rect.minX,s.rect.minY,s.rect.maxX,s.rect.maxY,s.rect.maxX-s.rect.minX,s.rect.maxY-s.rect.minY,s.viewportNDC.maxX-s.viewportNDC.minX,s.viewportNDC.maxY-s.viewportNDC.minY})
        if(!std::isfinite(v))throw std::invalid_argument("Nonfinite original world interaction screen metadata");
    if(s.nearZ<0 || !(s.ddcWidth>0) || !(s.ddcHeight>0) || !(s.viewportNDC.maxX>s.viewportNDC.minX) || !(s.viewportNDC.maxY>s.viewportNDC.minY) ||
        !(s.rect.maxX>s.rect.minX) || !(s.rect.maxY>s.rect.minY))throw std::invalid_argument("Invalid original world interaction screen extent");
    for(const auto& entry:transportInverses) {
        if(!entry.first)throw std::invalid_argument("Missing original transport collision GUID");const auto& m=entry.second;
        for(float v:{m.a0,m.a1,m.a2,m.a3,m.b0,m.b1,m.b2,m.b3,m.c0,m.c1,m.c2,m.c3,m.d0,m.d1,m.d2,m.d3})
            if(!std::isfinite(v))throw std::invalid_argument("Nonfinite original transport collision inverse");
        const float determinant=m.Determinant();if(!std::isfinite(determinant) || determinant==0)throw std::invalid_argument("Singular original transport collision inverse");
    }
}
WorldInteractionFrame::Hit WorldInteractionFrame::ResolveCollision(const Frame& completed,Hit hit,CollisionObject object) const {
    if(!Matches(completed))throw std::logic_error("Collision resolution does not belong to displayed image generation");
    if(hit.kind!=3)return hit;
    if(object==CollisionObject::Missing){hit.kind=0;hit.guid=0;return hit;}
    if(object==CollisionObject::Transport) {
        const auto inverse=transportInverses.find(hit.guid);
        if(inverse==transportInverses.end())throw std::logic_error("Displayed collision has no original transport transform");
        hit.point*=inverse->second;
        if(!std::isfinite(hit.point.x)||!std::isfinite(hit.point.y)||!std::isfinite(hit.point.z))throw std::invalid_argument("Displayed transport collision point overflow");
    }
    return hit;
}
bool WorldInteractionFrame::Depth(const Frame& completed,const C3Vector& point,float& depth) const {
    const auto p=actors->Project(completed,point);if(p.z<screen.nearZ || !p.w)return false;
    const float inv=1.f/p.w;
    float x=screen.ddcWidth*((p.x*inv+1.f)*.5f*(screen.viewportNDC.maxX-screen.viewportNDC.minX));
    float y=screen.ddcHeight*((p.y*inv+1.f)*.5f*(screen.viewportNDC.maxY-screen.viewportNDC.minY));
    if(screen.rect.minX<0)x-=screen.rect.minX;if(screen.rect.minY<0)y-=screen.rect.minY;
    if(!std::isfinite(x)||!std::isfinite(y))throw std::invalid_argument("Displayed world screen coordinate overflow");
    if(x<0 || x>screen.rect.maxX-screen.rect.minX || y<0 || y>screen.rect.maxY-screen.rect.minY)return false;
    depth=p.z;return true;
}
WorldInteractionFrame::Hit WorldInteractionFrame::Pick(CM2Scene& scratch,const Frame& completed,State& state,
    const C3Vector& start,const C3Vector& end,uint32_t flags,const std::vector<ActorPolicy>& policies) const {
    if(!Matches(completed) || state.owner!=this)throw std::logic_error("Interaction does not belong to displayed world generation");
    std::vector<const ActorPolicy*> choices(actors->RootCount());
    for(const auto& p:policies) {
        if(p.root>=choices.size() || choices[p.root])throw std::invalid_argument("Invalid or repeated displayed actor policy root");choices[p.root]=&p;
    }
    Hit result;C3Vector point=end,direction=end-start;
    float length=direction.x*direction.x+direction.y*direction.y+direction.z*direction.z;
    if(!std::isfinite(length))throw std::invalid_argument("Displayed world pick extent overflow");
    if(std::fabs(length)>=.00000023841858f) {length=std::sqrt(length);const float inv=1.f/length;direction.x*=inv;direction.y*=inv;direction.z*=inv;}
    float t=1;WorldCollisionFrame::Hit collision;uint64_t guid=0;bool hitWorld=world->Intersect(scratch,start,point,1,(flags&2)?0x1020124:0x1000124,collision);
    if(hitWorld){length*=collision.fraction;guid=collision.guid;point=C3Vector(direction.x*length+start.x,direction.y*length+start.y,direction.z*length+start.z);t=length;}
    uint64_t model=0;float objectDistance=0;
    if(flags&0x7c) {
        std::vector<M2HitRootSelection> normal,low;std::vector<uint32_t> retained,moved;
        for(auto root:state.order) {
            const auto* p=choices[root];if(p && p->lowPriority){low.push_back({root,p->priority});moved.push_back(root);}
            else {retained.push_back(root);if(p)normal.push_back({root,p->priority});}
        }
        model=actors->FindClosest(scratch,completed,normal,start,point,objectDistance);
        if(!model)model=actors->FindClosest(scratch,completed,low,start,point,objectDistance);
        retained.insert(retained.end(),moved.begin(),moved.end());state.order=std::move(retained);
        if(model){length=objectDistance;point=C3Vector(direction.x*objectDistance+start.x,direction.y*objectDistance+start.y,objectDistance*direction.z+start.z);}
    }
    const uint32_t terrain=flags&3;
    if(terrain) {
        t=1;
        if(world->Intersect(scratch,start,point,1,0x100151,collision)) {
            length*=collision.fraction;guid=collision.guid;hitWorld=true;
            point=C3Vector(direction.x*length+start.x,direction.y*length+start.y,direction.z*length+start.z);t=length;
        }
    }
    result.hasDepth=Depth(completed,point,result.depth);
    if(model){result.kind=2;result.distance=objectDistance;result.point=point;result.guid=model;return result;}
    if(!hitWorld)return result;
    result.point=point;result.distance=t;
    if(guid){result.kind=3;result.guid=guid;}else result.kind=terrain!=0;
    // Collision object existence/transport revalidation belongs to the UI owner;
    // it must use the displayed generation's captured transport matrix.
    return result;
}
bool WorldInteractionFrame::ScreenCoordinates(const Frame& frame,const C3Vector& point,C3Vector& out,uint32_t* flags) const {
    const auto p=actors->Project(frame,point);
    if(p.z<screen.nearZ || !p.w)return false;
    const float inv=1.f/p.w;
    out.x=screen.ddcWidth*(p.x*inv+1.f)*.5f*(screen.viewportNDC.maxX-screen.viewportNDC.minX);
    out.y=screen.ddcHeight*(p.y*inv+1.f)*.5f*(screen.viewportNDC.maxY-screen.viewportNDC.minY);
    out.z=p.z;
    if(screen.rect.minX<0)out.x-=screen.rect.minX;
    if(screen.rect.minY<0)out.y-=screen.rect.minY;
    const float width=screen.rect.maxX-screen.rect.minX,height=screen.rect.maxY-screen.rect.minY;
    const uint32_t result=(out.x>=0?1u:0u)|(out.y>=0?2u:0u)|(out.x<=width?4u:0u)|(out.y<=height?8u:0u);
    if(flags)*flags=result;
    return result==15;
}
