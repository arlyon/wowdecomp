#include "WmoGeometryHitSnapshot.h"
#include "MapObjGroup.h"
#include "MapObj.h"
#include "Engine/Source/Tempest/NTempest.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace {
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
float Axis(const C3Vector& p,uint32_t axis){return axis==0?p.x:axis==1?p.y:p.z;}
void Axis(C3Vector& p,uint32_t axis,float value){if(!axis)p.x=value;else if(axis==1)p.y=value;else p.z=value;}
template<class T> std::vector<T> Own(const T* data,size_t count) {
    if(!count)return {};if(!data)throw std::invalid_argument("Missing original WMO collision array");return {data,data+count};
}
}
std::shared_ptr<const WmoGeometryHitSnapshot> WmoGeometryHitSnapshot::Capture(const CMapObjGroup& source) {
    if(source.m_polyListSize<0 || source.m_indicesCount<0 || source.m_vertexListCount<0)throw std::invalid_argument("Negative original WMO collision extent");
    const auto& bsp=source.m_cAaBspNodePtr1;auto result=std::shared_ptr<WmoGeometryHitSnapshot>(new WmoGeometryHitSnapshot);
    result->nodes=Own(bsp.m_nodes,bsp.m_nNodes);result->leafFaces=Own(bsp.m_nodeFaceIndices,bsp.m_nNodeFaceIndices);
    result->faces=Own(source.m_polyList,source.m_polyListSize);result->indices=Own(source.m_indices,source.m_indicesCount);
    result->vertices=Own(source.m_vertexList,source.m_vertexListCount);result->bounds=bsp.m_aaBox;
    if(!Finite(result->bounds.b) || !Finite(result->bounds.t) || result->bounds.b.x>result->bounds.t.x ||
        result->bounds.b.y>result->bounds.t.y || result->bounds.b.z>result->bounds.t.z)
        throw std::invalid_argument("Invalid original WMO BSP bounds");
    if(result->faces.size()>result->indices.size()/3)throw std::invalid_argument("Original WMO face exceeds triangle allocation");
    for(const auto& p:result->vertices)if(!Finite(p))throw std::invalid_argument("Nonfinite original WMO collision vertex");
    for(auto index:result->indices)if(index>=result->vertices.size())throw std::invalid_argument("Original WMO triangle exceeds vertex allocation");
    if(source.m_parent && source.m_parent->m_materialsCount<0)throw std::invalid_argument("Negative original WMO material allocation");
    const uint32_t count=source.m_parent?uint32_t(source.m_parent->m_materialsCount):0;
    if(count && !source.m_parent->m_materialList)throw std::invalid_argument("Missing original WMO collision materials");
    result->blendModes.reserve(count);
    for(uint32_t i=0;i<count;++i)result->blendModes.push_back(source.m_parent->m_materialList[i].m_blendMode);
    for(const auto& face:result->faces)if(face.m_materialId!=0xff && face.m_materialId>=count)
        throw std::invalid_argument("Original WMO face exceeds material allocation");
    for(const auto& node:result->nodes) {
        if(node.m_flags&4) {
            if(uint64_t(node.m_faceStart)+node.m_nFaces>result->leafFaces.size())throw std::invalid_argument("Original WMO leaf exceeds face allocation");
        }else {
            if((node.m_flags&3)>=3 || !std::isfinite(node.m_planeDist))throw std::invalid_argument("Invalid original WMO split plane");
            for(auto child:{node.m_posChild,node.m_negChild})if(child!=0xffff && child>=result->nodes.size())throw std::invalid_argument("Original WMO child exceeds node allocation");
        }
    }
    for(auto face:result->leafFaces)if(face>=result->faces.size())throw std::invalid_argument("Original WMO BSP face exceeds triangle allocation");
    if(!result->nodes.empty()) {
        struct Visit{uint32_t node;bool leave;};std::vector<Visit> stack{{0,false}};std::vector<uint8_t> colour(result->nodes.size());
        while(!stack.empty()) {
            const auto visit=stack.back();stack.pop_back();
            if(visit.leave){colour[visit.node]=2;continue;}
            if(colour[visit.node]==1)throw std::invalid_argument("Cyclic original WMO BSP");
            if(colour[visit.node]==2)continue;colour[visit.node]=1;stack.push_back({visit.node,true});
            const auto& node=result->nodes[visit.node];if(node.m_flags&4)continue;
            for(auto child:{node.m_negChild,node.m_posChild})if(child!=0xffff)stack.push_back({child,false});
        }
    }
    return result;
}
bool WmoGeometryHitSnapshot::Intersect(const C3Segment& segment,float& fraction,uint16_t ignoreFlags,int32_t& hitFace) const {
    if(!Finite(segment.b) || !Finite(segment.t) || !std::isfinite(fraction))throw std::invalid_argument("Nonfinite owned WMO collision query");
    if(nodes.empty())return false;
    C3Ray ray;ray.origin=segment.b;ray.dir=segment.t-segment.b;
    const float length=std::sqrt(ray.dir.z*ray.dir.z+ray.dir.y*ray.dir.y+ray.dir.x*ray.dir.x);
    if(!std::isfinite(length))throw std::invalid_argument("WMO collision segment extent overflow");
    if(!length)return false;
    const float inverse=1.f/length;ray.dir.x*=inverse;ray.dir.y*=inverse;ray.dir.z=inverse*ray.dir.z;
    float maximum=fraction*length;const float original=fraction;
    if(!std::isfinite(maximum))throw std::invalid_argument("WMO collision limit overflow");
    ignoreFlags|=0x80;std::vector<uint8_t> visited(faces.size());bool hit=false;
    const auto test=[&](uint16_t face) {
        const auto& poly=faces[face];uint16_t bits;static_assert(sizeof(bits)==sizeof(poly));std::memcpy(&bits,&poly,sizeof(bits));
        if((ignoreFlags&bits) || visited[face])return;
        const uint16_t material=(poly.m_materialId==0xff || !blendModes[poly.m_materialId])?0x200:0x100;
        if(ignoreFlags&material)return;visited[face]=1;
        float distance=0;
        if(!NTempest::Intersect(ray,vertices.data(),indices.data()+3*size_t(face),&distance,nullptr,.0020000001f) || distance<0 || distance>maximum)return;
        maximum=distance;fraction=distance*inverse;if(fraction>original)fraction=original;hitFace=face;hit=true;
    };
    struct Work {uint32_t node;C3Segment segment;CAaBox box;};std::vector<Work> stack{{0,segment,bounds}};
    while(!stack.empty()) {
        const auto work=stack.back();stack.pop_back();const auto& node=nodes[work.node];
        if(node.m_flags&4){for(uint32_t i=0;i<node.m_nFaces;++i)test(leafFaces[size_t(node.m_faceStart)+i]);continue;}
        const uint32_t axis=node.m_flags&3;const float b=Axis(work.segment.b,axis),t=Axis(work.segment.t,axis);
        if(!(b-Axis(work.box.b,axis)>=-.0099999998f || t-Axis(work.box.b,axis)>=-.0099999998f) ||
            !(Axis(work.box.t,axis)-b>=.0099999998f || Axis(work.box.t,axis)-t>=.0099999998f))continue;
        auto positive=work.box,negative=work.box;Axis(positive.b,axis,node.m_planeDist);Axis(negative.t,axis,node.m_planeDist);
        const float db=b-node.m_planeDist,dt=t-node.m_planeDist;
        const auto push=[&](uint16_t child,const C3Segment& part,const CAaBox& box){if(child!=0xffff)stack.push_back({child,part,box});};
        if((db>=-.0099999998f && db<=.0099999998f) || (dt>=-.0099999998f && dt<=.0099999998f)) {
            push(node.m_negChild,work.segment,negative);push(node.m_posChild,work.segment,positive);
        }else if(db>.0099999998f && dt>.0099999998f)push(node.m_posChild,work.segment,positive);
        else if(db<-.0099999998f && dt<-.0099999998f)push(node.m_negChild,work.segment,negative);
        else {
            const auto split=work.segment.Lerp(db/(db-dt));C3Segment first,second;first.b=work.segment.b;first.t=split;second.b=split;second.t=work.segment.t;
            if(db<=0){push(node.m_posChild,second,positive);push(node.m_negChild,first,negative);}
            else {push(node.m_negChild,second,negative);push(node.m_posChild,first,positive);}
        }
    }
    return hit;
}
std::shared_ptr<const WmoGeometryHitSnapshot> CMapObjGroup::CaptureGeometryHitSnapshot() {
    if(!m_geometryHitSnapshot)m_geometryHitSnapshot=WmoGeometryHitSnapshot::Capture(*this);return m_geometryHitSnapshot;
}
