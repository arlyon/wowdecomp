#include "WorldModelPickFrame.h"
#include "Engine/Source/Services/Camera.h"
#include <cmath>
#include <stdexcept>
namespace {
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
C44Matrix Matrix(const std::array<float,16>& v){
    for(float x:v)if(!std::isfinite(x))throw std::invalid_argument("Nonfinite displayed world camera");
    return C44Matrix(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13],v[14],v[15]);
}
}
WorldModelPickFrame::WorldModelPickFrame(GxWorld::WorldFrameLease id,C3Vector position,CRect rect,std::shared_ptr<const M2HitSceneSnapshot> geometry)
    :identity(id),camera(position),viewport(rect),models(std::move(geometry)){
    if(!identity.generation || !models || !Finite(camera) || !(viewport.maxX>viewport.minX) || !(viewport.maxY>viewport.minY))
        throw std::invalid_argument("Incomplete displayed world model snapshot");
}
bool WorldModelPickFrame::LineSegment(const GxWorld::WorldFrame& frame,float mouseX,float mouseY,C3Vector& start,C3Vector& end) const{
    if(!Matches(frame))return false;
    const float x=(mouseX-viewport.minX)/(viewport.maxX-viewport.minX),y=(mouseY-viewport.minY)/(viewport.maxY-viewport.minY);
    if(!(x>=0&&x<=1&&y>=0&&y<=1))return false;
    CameraGetLineSegmentFromTransforms(x,y,Matrix(frame.description.view),Matrix(frame.description.projection),&start,&end);
    start+=camera;end+=camera;
    if(!Finite(start)||!Finite(end))throw std::invalid_argument("Displayed camera ray overflow");
    return true;
}
C4Vector WorldModelPickFrame::Project(const GxWorld::WorldFrame& frame,const C3Vector& point) const{
    if(!Matches(frame)||!Finite(point))throw std::logic_error("Projection does not belong to the displayed image");
    const auto d=point-camera;C4Vector p;p.x=d.x;p.y=d.y;p.z=d.z;p.w=0;
    const auto result=p*(Matrix(frame.description.view)*Matrix(frame.description.projection));
    if(!std::isfinite(result.x)||!std::isfinite(result.y)||!std::isfinite(result.z)||!std::isfinite(result.w))throw std::invalid_argument("Displayed projection overflow");
    return result;
}
uint64_t WorldModelPickFrame::FindClosest(CM2Scene& scene,const GxWorld::WorldFrame& frame,const std::vector<M2HitRootSelection>& roots,const C3Vector& start,const C3Vector& end,float& distance) const{
    if(!Matches(frame))throw std::logic_error("Picking does not belong to the displayed image");
    const auto view=Matrix(frame.description.view);const auto a=(start-camera)*view,b=(end-camera)*view;
    float t=1;const auto owner=models->Query(scene,roots,a,b,t);
    if(owner){const auto d=b-a;distance=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z)*t;}
    return owner;
}
