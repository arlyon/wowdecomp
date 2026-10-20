#include "M2ShadowMesh.h"
#include "ShadowSelection.h"
#include "M2Cache.h"
#include "M2Model.h"
#include "M2Scene.h"
#include "M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cmath>
#include <cstring>
#include <vector>
namespace {
bool Finite(const C3Vector& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
bool Ready(CM2Model& model){
    if(!model.m_shared || (model.m_shared->m_flags&3)!=3)return false;
    if(!(model.m_fFlags&1))model.InitializeLoaded();
    return (model.m_fFlags&1)!=0;
}
}
void PrepareM2ShadowPoses(CM2Scene& scene){
    // Queue into the original animator BEFORE its one normal frame update.
    for(auto* model=scene.m_modelList;model;model=model->m_sceneNext){
        if(model->m_fFlags&0x4000 || !Ready(*model))continue;
        const auto& data=*model->m_shared->m_data;const auto& skin=*model->m_shared->m_skinData;
        bool potential=false;
        for(uint32_t i=0;i<skin.m_batches.m_count;++i){const auto& batch=skin.m_batches[i];
            if(M2Shadow::BatchExclusion(batch.m_shader,batch.m_flags,batch.m_materialLayer))continue;
            if(batch.m_materialIndex>=data.m_materials.m_count)continue;
            const auto& material=data.m_materials[batch.m_materialIndex];
            if(!M2Shadow::MaterialExclusion(material.m_flags,material.m_blendMode)){potential=true;break;}
        }
        if(!potential)continue;
        std::vector<CM2Model*> chain;
        for(auto* p=model;p;p=p->m_attachParent){if(!Ready(*p)){chain.clear();break;}chain.push_back(p);}
        for(auto i=chain.rbegin();i!=chain.rend();++i)if(!(*i)->m_animatePrev)(*i)->SetAnimating(1);
    }
}
GxShadow::FrameResult BuildM2ShadowMesh(const CM2Model& model,const CM2Scene& scene,uint32_t batchIndex,GxShadow::Mesh& out){
    using namespace GxShadow;out={};out.kind=GeometryKind::Model;
    if(model.m_fFlags&0x4000)return FrameResult::Ready;
    if(!(model.m_fFlags&1)||!model.m_shared||!model.m_shared->m_data||!model.m_shared->m_skinData||!model.m_shared->m_cache)return FrameResult::PendingGeometry;
    const auto& data=*model.m_shared->m_data;const auto& skin=*model.m_shared->m_skinData;
    const bool optimized=model.m_optGeo!=nullptr;
    const uint32_t batchCount=optimized?model.m_optGeo->m_batchCount:skin.m_batches.m_count;
    if(batchIndex>=batchCount)return FrameResult::InvalidGeometry;
    const auto& batch=optimized?model.m_optGeo->m_batches[batchIndex]:skin.m_batches[batchIndex];
    if(M2Shadow::BatchExclusion(batch.m_shader,batch.m_flags,batch.m_materialLayer))return FrameResult::Ready;
    const uint32_t sectionCount=optimized?model.m_optGeo->m_skinSectionCount:skin.m_skinSections.m_count;
    if(batch.m_skinSectionIndex>=sectionCount||batch.m_materialIndex>=data.m_materials.m_count)return FrameResult::InvalidGeometry;
    if(!optimized && (!model.m_skinSections||!model.m_skinSections[batch.m_skinSectionIndex]))return FrameResult::Ready;
    const auto& material=data.m_materials[batch.m_materialIndex];
    if(M2Shadow::MaterialExclusion(material.m_flags,material.m_blendMode))return FrameResult::Ready;
    if(model.m_scene!=&scene || model.m_frameStamp!=scene.m_frameStamp)return FrameResult::StalePose;
    if((batch.m_colorIndex<data.m_colors.m_count&&!model.m_colors)||
        (batch.m_textureCount && (batch.m_textureWeightComboIndex>=data.m_textureWeightCombos.m_count ||
        data.m_textureWeightCombos[batch.m_textureWeightComboIndex]>=data.m_textureWeights.m_count || !model.m_textureWeights)))return FrameResult::InvalidGeometry;
    const float opacity=M2Shadow::BatchOpacity(model,batch);
    if(!std::isfinite(opacity))return FrameResult::InvalidGeometry;
    const auto selected=Describe(M2Shadow::MaterialCoverage(material.m_flags,material.m_blendMode,opacity));
    if(!selected.casts)return FrameResult::Ready;
    const auto& section=optimized?model.m_optGeo->m_skinSections[batch.m_skinSectionIndex]:skin.m_skinSections[batch.m_skinSectionIndex];
    if(!section.m_indexCount)return FrameResult::Ready;
    // Optimised topology lives in its original index buffer, not the profile.
    // Until its CPU topology is exposed, never pretend it was collected.
    if(optimized)return FrameResult::UnsupportedCoverage;
    if(!model.m_boneMatrices||section.m_boneInfluences>4||section.m_indexCount%3 ||
        uint32_t(section.m_vertexStart)+section.m_vertexCount>skin.m_vertices.m_count ||
        uint32_t(section.m_indexStart)+section.m_indexCount>skin.m_indices.m_count)return FrameResult::InvalidGeometry;
    out.material.mode=selected.mode;out.material.opacity=opacity;out.material.twoSided=(material.m_flags&4)!=0;
    out.material.threshold=selected.mode==CoverageMode::Cutout?opacity*(224.f/255.f):128.f/255.f;
    uint32_t coord=0,transform=UINT32_MAX;
    if(selected.needsCoverageTexture){
        if(batch.m_textureCount!=1)return FrameResult::UnsupportedCoverage;
        if(batch.m_textureComboIndex>=data.m_textureCombos.m_count || batch.m_textureCoordComboIndex>=data.m_textureCoordCombos.m_count || batch.m_textureTransformComboIndex>=data.m_textureTransformCombos.m_count)return FrameResult::InvalidGeometry;
        const uint32_t texture=data.m_textureCombos[batch.m_textureComboIndex];
        if(texture>=data.m_textures.m_count || !model.m_textures)return FrameResult::InvalidGeometry;
        coord=data.m_textureCoordCombos[batch.m_textureCoordComboIndex];if(coord>1)return FrameResult::UnsupportedCoverage;
        transform=data.m_textureTransformCombos[batch.m_textureTransformComboIndex];
        if(transform<data.m_textureTransforms.m_count&&!model.m_textureMatrices)return FrameResult::StalePose;
        auto* binding=model.m_textures[texture];
        auto* native=binding?TextureGetGxTex(binding,0,nullptr):nullptr;
        if(!native)return FrameResult::PendingTexture;
        auto retained=HandleDuplicate(reinterpret_cast<HOBJECT>(binding));
        out.material.textureOwners[0]=std::shared_ptr<const void>(reinterpret_cast<const void*>(retained),[](const void* p){HandleClose(reinterpret_cast<HOBJECT>(const_cast<void*>(p)));});
        out.material.textures[0]=native;out.material.textureCount=1;
        out.material.wrapU[0]=(data.m_textures[texture].m_flags&1)!=0;out.material.wrapV[0]=(data.m_textures[texture].m_flags&2)!=0;
    }
    const bool hardware=(model.m_shared->m_cache->m_flags&8)!=0;
    out.positions.reserve(section.m_vertexCount);out.coverage.reserve(section.m_vertexCount);
    constexpr uint32_t order[4]={1,0,2,3};
    for(uint32_t j=section.m_vertexStart;j<uint32_t(section.m_vertexStart)+section.m_vertexCount;++j){
        const uint32_t source=hardware?skin.m_vertices[j]:j;if(source>=data.m_vertices.m_count)return FrameResult::InvalidGeometry;
        const auto& vertex=data.m_vertices[source];if(!Finite(vertex.m_position))return FrameResult::InvalidGeometry;
        C44Matrix blended;
        blended.a0=blended.a1=blended.a2=blended.a3=0;
        blended.b0=blended.b1=blended.b2=blended.b3=0;
        blended.c0=blended.c1=blended.c2=blended.c3=0;
        blended.d0=blended.d1=blended.d2=0;blended.d3=1;
        const bool rigid=hardware?section.m_boneInfluences<=1:section.m_boneInfluences==1;
        for(uint32_t slot=0;slot<(rigid?1u:4u);++slot){
            const uint32_t k=hardware&&!rigid?order[slot]:slot;
            if(!hardware&&k&&!vertex.m_weights.b[k])break;
            uint32_t bone=vertex.m_indices.b[k];
            if(hardware){
                if(section.m_boneInfluences&&j>=skin.m_bones.m_count)return FrameResult::InvalidGeometry;
                const uint32_t local=section.m_boneInfluences?skin.m_bones[j].b[k]:0;
                if(local>=section.m_boneCount || uint32_t(section.m_boneComboIndex)+local>=data.m_boneCombos.m_count)return FrameResult::InvalidGeometry;
                bone=data.m_boneCombos[section.m_boneComboIndex+local];
            }
            if(bone>=data.m_bones.m_count)return FrameResult::InvalidGeometry;
            const float weight=rigid?1.f:vertex.m_weights.b[k]*.0039215689f;
            const float* m=&model.m_boneMatrices[bone].a0;float* b=&blended.a0;
            for(unsigned row=0;row<4;++row)for(unsigned col=0;col<3;++col)b[row*4+col]+=m[row*4+col]*weight;
        }
        const auto position=(vertex.m_position*blended)*scene.m_viewInv;
        if(!Finite(position))return FrameResult::InvalidGeometry;
        out.positions.push_back({{position.x,position.y,position.z}});
        CoverageVertex coverage;
        if(selected.needsCoverageTexture){
            C3Vector uv(vertex.m_texcoord[coord].x,vertex.m_texcoord[coord].y,0);
            if(transform<data.m_textureTransforms.m_count)uv=uv*model.m_textureMatrices[transform];
            if(!Finite(uv))return FrameResult::InvalidGeometry;
            coverage.uv[0][0]=uv.x;coverage.uv[0][1]=uv.y;
        }
        out.coverage.push_back(coverage);
    }
    for(uint32_t j=section.m_indexStart;j<uint32_t(section.m_indexStart)+section.m_indexCount;++j){
        const uint32_t index=skin.m_indices[j];
        if(index<section.m_vertexStart||index>=uint32_t(section.m_vertexStart)+section.m_vertexCount)return FrameResult::InvalidGeometry;
        out.indices.push_back(index-section.m_vertexStart);
    }
    return FrameResult::Ready;
}
