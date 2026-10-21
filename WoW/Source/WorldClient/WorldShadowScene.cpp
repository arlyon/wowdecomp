#include "WorldShadowScene.h"
#include "Map.h"
#include "MapArea.h"
#include "MapChunk.h"
#include "MapObj.h"
#include "MapObjDef.h"
#include "MapObjGroup.h"
#include "World.h"
#include "CWorldMath.h"
#include "MapLight.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2ShadowMesh.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Base/Handle.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
namespace {
using namespace GxShadow;
bool Intersects(const CAaBox& box,const Frame& frame){
    const float extent=frame.regionRadius+frame.rayDistance;
    return box.b.x<=frame.camera[0]+extent&&box.t.x>=frame.camera[0]-extent&&
        box.b.y<=frame.camera[1]+extent&&box.t.y>=frame.camera[1]-extent&&
        box.b.z<=frame.camera[2]+extent&&box.t.z>=frame.camera[2]-extent;
}
void Failure(Frame& frame,FrameResult result){if(result==FrameResult::Ready)return;
    if(result==FrameResult::UnsupportedCoverage)++frame.unsupported;else ++frame.unready;
    if(frame.result==FrameResult::Ready||result==FrameResult::InvalidGeometry)frame.result=result;
}
void Append(Frame& frame,Mesh&& mesh){if(mesh.indices.empty())return;frame.triangles+=mesh.indices.size()/3;frame.meshes.push_back(std::move(mesh));}
std::shared_ptr<const void> Retain(CTexture* texture){auto handle=HandleDuplicate(reinterpret_cast<HOBJECT>(texture));return {reinterpret_cast<const void*>(handle),[](const void* p){HandleClose(reinterpret_cast<HOBJECT>(const_cast<void*>(p)));}};}
}
std::shared_ptr<const GxShadow::Frame> CollectWorldShadowScene(const C3Vector& camera,const C44Matrix& projection){
    using namespace GxShadow;
    const auto started=std::chrono::steady_clock::now();
    auto frame=std::make_shared<Frame>();frame->result=FrameResult::Ready;frame->camera={camera.x,camera.y,camera.z};
    auto* scene=World::s_m2Scene;if(!scene||!CMap::s_mapLight){frame->result=FrameResult::PendingGeometry;return frame;}
    C44Matrix vp=scene->m_view*projection;
    const float determinant=vp.Determinant();
    if(!std::isfinite(determinant)||std::abs(determinant)<1e-12f){frame->result=FrameResult::InvalidGeometry;return frame;}
    std::memcpy(frame->viewInverse.data(),&scene->m_viewInv.a0,64);
    const auto inverse=vp.Inverse(determinant);std::memcpy(frame->inverseViewProjection.data(),&inverse.a0,64);
    const auto& direction=CMap::s_mapLight->m_light.m_dir;
    frame->lightDirection={-direction.x,-direction.y,-direction.z};
    frame->viewportDepthRange={g_theGxDevicePtr->m_viewport.m_z.m_l,g_theGxDevicePtr->m_viewport.m_z.m_h};
    // Original loaded lists, never frustum-only draw lists or forced asset loads.
    for(auto* chunk=CMap::s_mapChunkList.Head();chunk;chunk=CMap::s_mapChunkList.Next(chunk)){
        if(!(chunk->m_flags&MAPOBJ_FLAG_PREPARED)||!Intersects(chunk->m_bbox,*frame))continue;
        auto* link=chunk->m_parentLinkList.Head();auto* area=link?static_cast<CMapArea*>(link->m_ref):nullptr;
        if(!area||area->m_asyncObject)continue;
        if(!chunk->m_header||!chunk->m_height){Failure(*frame,FrameResult::InvalidGeometry);continue;}
        Mesh mesh;uint16_t topology[768];const auto count=chunk->CreateIndices(reinterpret_cast<char*>(topology),0);
        if(count<0||count>768||count%3){Failure(*frame,FrameResult::InvalidGeometry);continue;}
        for(int i=0;i<145;++i){C3Vector position;chunk->GetVertexPosition(i,&position);mesh.positions.push_back({{position.x,position.y,position.z}});}
        for(int i=0;i<count;++i){if(topology[i]>=145){Failure(*frame,FrameResult::InvalidGeometry);break;}mesh.indices.push_back(topology[i]);}
        ++frame->terrain;Append(*frame,std::move(mesh));
    }
    for(auto* model=scene->m_modelList;model;model=model->m_sceneNext){
        if(!(model->m_fFlags&1)||!model->m_shared||!model->m_shared->m_data||!model->m_shared->m_skinData)continue;
        C44Matrix placement=model->m_matrixF4*scene->m_viewInv;CAaBox box;
        CWorldMath::TransformAABox(placement,model->m_shared->m_boundingBox,box);if(!Intersects(box,*frame))continue;
        ++frame->models;
        const auto count=model->m_shared->m_skinData->m_batches.m_count;
        for(uint32_t i=0;i<count;++i){Mesh mesh;const auto result=BuildM2ShadowMesh(*model,*scene,i,mesh);
            if(result!=FrameResult::Ready){
                const auto& profile=*model->m_shared->m_skinData;
                const auto& batch=profile.m_batches[i];
                std::string reason;
                switch(result){
                case FrameResult::UnsupportedCoverage:reason=batch.m_textureCount!=1?"cutout has multiple coverage textures":"cutout uses unsupported coverage UV";break;
                case FrameResult::PendingGeometry:reason="geometry not ready";break;
                case FrameResult::PendingTexture:reason="coverage texture not ready";break;
                case FrameResult::StalePose:reason="current pose unavailable";break;
                case FrameResult::InvalidGeometry:reason="invalid geometry or material indices";break;
                default:reason="collector result="+std::to_string(unsigned(result));break;
                }
                const std::string issue=std::string("asset=")+model->m_shared->m_fileName+" batch="+std::to_string(i)+
                    " material="+std::to_string(batch.m_materialIndex)+" textures="+std::to_string(batch.m_textureCount)+
                    " shader="+std::to_string(batch.m_shader)+" optimized="+std::to_string(model->m_optGeo!=nullptr)+" reason="+reason;
                ++frame->modelIssues[issue];
            }
            Failure(*frame,result);if(result==FrameResult::Ready){if(mesh.indices.empty())++frame->nonCasters;Append(*frame,std::move(mesh));}}
    }
    for(auto* placed=CMap::mapObjDefGroupList.Head();placed;placed=CMap::mapObjDefGroupList.Next(placed)){
        auto* link=placed->m_parentLinkList.Head();auto* def=link?static_cast<CMapObjDef*>(link->m_ref):nullptr;auto* owner=def?def->m_owner:nullptr;
        if(!owner||placed->m_groupNum>=uint32_t(owner->m_mapObjGroupCount))continue;
        auto* group=owner->m_mapObjGroupArray[placed->m_groupNum];if(!group||!(group->m_unkLoadedFlag&1))continue;
        CAaBox box;CWorldMath::TransformAABox(def->m_mat,group->m_bbox,box);if(!Intersects(box,*frame))continue;++frame->buildings;
        if(group->m_vertexListCount<0||!group->m_vertexList||group->m_indicesCount<0||!group->m_indices){Failure(*frame,FrameResult::InvalidGeometry);continue;}
        for(int b=0;b<group->m_batchListCount;++b){const auto& batch=group->m_batchList[b];
            if(batch.m_texture>=owner->m_materialsCount||batch.m_indexCount%3||uint64_t(batch.m_indexStart)+batch.m_indexCount>uint32_t(group->m_indicesCount)){Failure(*frame,FrameResult::InvalidGeometry);continue;}
            auto& material=owner->m_materialList[batch.m_texture];Mesh mesh;mesh.kind=GeometryKind::Building;mesh.material.twoSided=(material.m_flags&4)!=0;
            if(material.m_blendMode==1){
                auto* texture=material.Textures()[0];auto* native=texture?TextureGetGxTex(texture,0,nullptr):nullptr;
                if(!native){Failure(*frame,FrameResult::PendingTexture);continue;}
                if(!group->m_textureVertexList||group->m_textureVertexListCount!=group->m_vertexListCount){Failure(*frame,FrameResult::InvalidGeometry);continue;}
                mesh.material.mode=CoverageMode::Cutout;mesh.material.threshold=224.f/255;mesh.material.textureCount=1;mesh.material.textures[0]=native;mesh.material.textureOwners[0]=Retain(texture);
                mesh.material.wrapU[0]=!(material.m_flags&0x40);mesh.material.wrapV[0]=!(material.m_flags&0x80);
            }
            for(int i=0;i<group->m_vertexListCount;++i){const auto p=group->m_vertexList[i]*def->m_mat;mesh.positions.push_back({{p.x,p.y,p.z}});CoverageVertex uv;if(mesh.material.textureCount){uv.uv[0][0]=group->m_textureVertexList[i].x;uv.uv[0][1]=group->m_textureVertexList[i].y;}mesh.coverage.push_back(uv);}
            bool valid=true;for(uint32_t i=0;i<batch.m_indexCount;++i){const uint32_t index=group->m_indices[batch.m_indexStart+i];if(index>=uint32_t(group->m_vertexListCount)){valid=false;break;}mesh.indices.push_back(index);}
            if(valid)Append(*frame,std::move(mesh));else Failure(*frame,FrameResult::InvalidGeometry);
        }
    }
    if(!frame->modelIssues.empty()){
        uint64_t occurrences=0;for(const auto& issue:frame->modelIssues)occurrences+=issue.second;
        frame->detail="Rejected M2 caster batches: "+std::to_string(frame->modelIssues.size())+" unique, "+std::to_string(occurrences)+" instances. Full asset/batch/reason list in client.log";
    }
    frame->collectionMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return frame;
}
