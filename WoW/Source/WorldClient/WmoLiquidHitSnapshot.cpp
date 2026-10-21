#include "WmoLiquidHitSnapshot.h"
#include "MapObjGroup.h"
#include "Engine/Source/Tempest/CMath.h"
#include "Engine/Source/Tempest/NTempest.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include <cmath>
#include <limits>
#include <stdexcept>
namespace {
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
size_t Product(uint32_t x,uint32_t y){if(y && x>std::numeric_limits<size_t>::max()/y)throw std::overflow_error("WMO liquid allocation extent overflow");return size_t(x)*y;}
int32_t Round(float v){if(!std::isfinite(v) || double(v)<double(INT32_MIN) || double(v)>double(INT32_MAX))throw std::invalid_argument("WMO liquid coordinate is not representable");return CMath::fint_(v);}
template<class F> void Visit(const C3Vector& start,const C3Vector& end,uint32_t width,uint32_t height,F visit) {
    const auto emit=[&](int32_t x,int32_t y){return x>=0 && y>=0 && uint32_t(x)<width && uint32_t(y)<height && visit(uint32_t(x),uint32_t(y));};
    const float dx=end.x-start.x,dy=std::fabs(end.y-start.y);
    if(dy<2.3841858e-7f || std::fabs(dx)<2.3841858e-7f) {
        const bool xMajor=dy<2.3841858e-7f;
        int32_t value=Round((xMajor?start.x:start.y)-.5f);const int32_t fixed=Round((xMajor?start.y:start.x)-.5f),last=Round((xMajor?end.x:end.y)-.5f),step=last>=value?1:-1;
        for(;;value+=step){if(!(xMajor?emit(value,fixed):emit(fixed,value)) || value==last)return;}
    }
    const bool xMajor=dy<std::fabs(dx);
    const float a=xMajor?start.x:start.y,b=xMajor?end.x:end.y,other=xMajor?start.y:start.x,otherEnd=xMajor?end.y:end.x;
    int32_t value=Round(a-.5f),previous=Round(other-.5f),current=previous;
    const int32_t step=a>b?-1:1;const int64_t last=int64_t(Round(b-.5f))+step;
    const float frac=step<0?float(value+1)-a:a-float(value),slope=(otherEnd-other)/std::fabs(b-a),base=other-frac*slope;
    float offset=0;if(int64_t(value)==last)return;
    do {
        if(current!=previous && !(xMajor?emit(value,previous):emit(previous,value)))return;
        offset+=slope;if(!(xMajor?emit(value,current):emit(current,value)))return;
        value+=step;previous=current;current=Round(offset+base-.5f);
    }while(int64_t(value)!=last);
    const uint32_t limit=xMajor?height:width;
    if(current!=previous && current>=0 && uint32_t(current)<limit) {
        if(xMajor)emit(value-step,current);else emit(current,value-step);
    }
}
}
std::shared_ptr<const WmoLiquidHitSnapshot> WmoLiquidHitSnapshot::Capture(const CMapObjGroup& source,bool flag4) {
    auto result=std::shared_ptr<WmoLiquidHitSnapshot>(new WmoLiquidHitSnapshot);result->typeFlag4=flag4;
    if(!source.m_liquidType || !source.m_liquidTiles.x || !source.m_liquidTiles.y)return result;
    if(source.m_liquidTiles.x<0 || source.m_liquidTiles.y<0 || source.m_liquidVerts.x<=0 || source.m_liquidVerts.y<=0 ||
        !source.m_liquidVertArray || !source.m_liquidTileList || !Finite(source.m_liquidCorner) ||
        !std::isfinite(source.m_liquidHeight) || !std::isfinite(source.m_liquidMaxHeight) || source.m_liquidHeight>source.m_liquidMaxHeight)
        throw std::invalid_argument("Invalid original WMO liquid source");
    result->width=source.m_liquidTiles.x;result->height=source.m_liquidTiles.y;result->stride=source.m_liquidVerts.x;result->corner=source.m_liquidCorner;
    const auto& data=source.m_liquidVertArray->m_verts;
    const auto vertexCount=Product(source.m_liquidVerts.x,source.m_liquidVerts.y);
    if(vertexCount>data.m_count || (vertexCount && !data.m_data))throw std::invalid_argument("Original WMO liquid exceeds vertex allocation");
    if(vertexCount)result->vertices.assign(data.m_data,data.m_data+vertexCount);
    for(const auto& v:result->vertices)if(!Finite(v))throw std::invalid_argument("Nonfinite original WMO liquid vertex");
    const auto count=Product(result->width,result->height);result->tiles.resize(count);
    for(size_t i=0;i<count;++i) {
        result->tiles[i]=source.m_liquidTileList[i].m_flags;
        if((result->tiles[i]&15)==15)continue;
        const size_t base=i%result->width+(i/result->width)*result->stride;
        if(base>=vertexCount || size_t(result->stride)+1>vertexCount-1-base)throw std::invalid_argument("Original WMO liquid triangle exceeds vertex allocation");
    }
    const float x=result->width*4.1666665f-.1f,y=result->height*4.1666665f-.1f;
    result->bounds.b=C3Vector(result->corner.x,result->corner.y,source.m_liquidHeight);
    result->bounds.t=C3Vector((x>0?x:0)+result->corner.x,(y>0?y:0)+result->corner.y,source.m_liquidMaxHeight);
    if(!Finite(result->bounds.t))throw std::invalid_argument("Original WMO liquid box overflow");return result;
}
bool WmoLiquidHitSnapshot::Intersect(const C3Segment& segment,float& fraction,uint32_t flags) const {
    if(!Finite(segment.b) || !Finite(segment.t) || !std::isfinite(fraction))throw std::invalid_argument("Nonfinite WMO liquid query");
    if(!(flags&0x30000) || !width || !height || ((flags&0x10000) && !(flags&0x20000) && !typeFlag4))return false;
    C3Vector start=segment.b,end=segment.t,direction=end-start;
    const float squared=direction.z*direction.z+direction.y*direction.y+direction.x*direction.x;
    if(!std::isfinite(squared))throw std::invalid_argument("WMO liquid segment extent overflow");
    if(!squared)return false;
    if(squared>2.3841858e-7f){const float inv=1.f/std::sqrt(squared);direction.x*=inv;direction.y*=inv;direction.z*=inv;}
    if(!bounds.ContainsPoint(start)) {C3Ray ray;ray.origin=segment.b;ray.dir=direction;float t=1;if(!NTempest::Intersect(ray,bounds,&t,&start))return false;}
    if(!bounds.ContainsPoint(end)) {C3Ray ray;ray.origin=end;direction.x=-direction.x;direction.y=-direction.y;direction.z=-direction.z;ray.dir=direction;float t=1;if(!NTempest::Intersect(ray,bounds,&t,&end))return false;}
    start.x=(start.x-corner.x)*.24f;start.y=(start.y-corner.y)*.24f;start.z*=.24f;
    end.x=(end.x-corner.x)*.24f;end.y=(end.y-corner.y)*.24f;end.z*=.24f;
    C3Ray ray;ray.origin=segment.b;ray.dir=segment.t-segment.b;
    const float length=std::sqrt(ray.dir.x*ray.dir.x+ray.dir.z*ray.dir.z+ray.dir.y*ray.dir.y);
    if(!std::isfinite(length) || !length)throw std::invalid_argument("WMO liquid triangle ray extent overflow");
    const float inv=1.f/length;ray.dir.x*=inv;ray.dir.y*=inv;ray.dir.z=inv*ray.dir.z;bool hit=false;
    Visit(start,end,width,height,[&](uint32_t x,uint32_t y) {
        if((tiles[size_t(y)*width+x]&15)==15)return true;
        const size_t base=x+size_t(y)*stride;
        if(base+stride+1>UINT32_MAX)throw std::overflow_error("WMO liquid triangle index is not representable");
        const uint32_t tri[2][3]{{uint32_t(base),uint32_t(base+stride+1),uint32_t(base+stride)},{uint32_t(base),uint32_t(base+1),uint32_t(base+stride+1)}};
        for(const auto& indices:tri) {
            float t=1;if(!NTempest::Intersect(ray,vertices.data(),indices,&t,nullptr,.0099999998f))continue;t*=inv;
            if(t>=fraction || t<0)continue;fraction=t;hit=true;
        }
        return true;
    });return hit;
}
std::shared_ptr<const WmoLiquidHitSnapshot> CMapObjGroup::CaptureLiquidHitSnapshot() {
    if(!(m_flags&0x1000) || !m_liquidType || !m_liquidTiles.x || !m_liquidTiles.y)return {};
    const auto* type=g_liquidTypeDB.GetRecord(m_liquidType);if(!type)throw std::invalid_argument("Missing original WMO liquid type");
    return WmoLiquidHitSnapshot::Capture(*this,(type->m_flags&4)!=0);
}
