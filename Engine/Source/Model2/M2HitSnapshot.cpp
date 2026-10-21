#include "M2HitSnapshot.h"
#include "M2Scene.h"
#include "M2Model.h"
#include "M2Shared.h"
#include "M2Cache.h"
#include "Engine/Source/Tempest/C33Matrix.h"
#include "Engine/Source/Tempest/C3Ray.h"
#include "Engine/Source/Tempest/NTempest.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
class M2HitGeometrySource {
public:
    std::vector<M2Vertex> vertices;
    std::vector<uint16_t> mapping,indices,collisionIndices;
    std::vector<M2SkinSection> sections;
    std::vector<C3Vector> collisionPositions;
    uint32_t boneCount=0;
};
namespace {
template<class T> std::vector<T> Own(const M2Array<T>& source) {
    if(!source.m_count)return {};
    if(!source.m_offset)throw std::invalid_argument("Missing original M2 hit array");
    return {source.Data(),source.Data()+source.m_count};
}
bool Finite(const C3Vector& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
void ValidateMatrix(const C44Matrix& m) {
    for(float v:{m.a0,m.a1,m.a2,m.a3,m.b0,m.b1,m.b2,m.b3,m.c0,m.c1,m.c2,m.c3,m.d0,m.d1,m.d2,m.d3})
        if(!std::isfinite(v))throw std::invalid_argument("Nonfinite original M2 collision transform");
}
std::shared_ptr<const M2HitGeometrySource> OwnGeometry(CM2Shared& shared) {
    if(!shared.m_hitGeometrySnapshot) {
        const auto& data=*shared.m_data;auto source=std::make_shared<M2HitGeometrySource>();source->boneCount=data.m_bones.m_count;
        source->vertices=Own(data.m_vertices);source->collisionPositions=Own(data.m_collisionPositions);source->collisionIndices=Own(data.m_collisionIndices);
        if(shared.m_skinData) {
            source->mapping=Own(shared.m_skinData->m_vertices);source->indices=Own(shared.m_skinData->m_indices);source->sections=Own(shared.m_skinData->m_skinSections);
        }
        shared.m_hitGeometrySnapshot=std::move(source);
    }
    return shared.m_hitGeometrySnapshot;
}
struct Scratch {
    CM2Scene& scene;C3Vector* original;uint32_t capacity;std::vector<C3Vector> owned;
    static size_t Count(size_t n) {
        if(n>UINT32_MAX)throw std::overflow_error("M2 hit scratch extent is not representable");return n;
    }
    Scratch(CM2Scene& s,size_t count):scene(s),original(s.m_hitVerts),capacity(s.m_hitVertCapacity),owned(Count(count)) {
        scene.m_hitVerts=owned.data();scene.m_hitVertCapacity=uint32_t(count);
    }
    ~Scratch(){scene.m_hitVerts=original;scene.m_hitVertCapacity=capacity;}
};
}
std::shared_ptr<const M2HitSnapshot> M2CaptureHitSnapshot(CM2Scene& scene,const CM2Model& model,uint32_t mode) {
    if(!(model.m_fFlags&1) || model.m_frameStamp==UINT32_MAX)return {};
    const bool collision=mode==3;
    if(model.m_frameStamp!=scene.m_frameStamp && (!collision || model.m_attachParent))return {};
    if(!model.m_shared || !model.m_shared->m_data)throw std::invalid_argument("Missing original M2 hit source");
    auto& shared=*model.m_shared;const auto& data=*shared.m_data;
    // Match HitTestGeometry's empty batch loop: an emitter/empty model has
    // nothing to pick and need not allocate section visibility or skinning
    // pose arrays. Collision queries still use their independent topology.
    if(!collision && shared.m_skinData && !shared.m_skinData->m_batches.m_count)return {};
    if(!collision && (!shared.m_skinData || !model.m_bones || !model.m_boneMatrices || !model.m_skinSections)) {
        throw std::invalid_argument("Missing evaluated original M2 hit pose");
    }
    const M2Bounds* bounds=&data.m_collisionBounds;
    if(!collision) {
        const auto sequence=model.m_bones->m_sequence.m_sequenceIndex;
        if(sequence>=data.m_sequences.m_count)throw std::invalid_argument("M2 hit sequence exceeds original source");
        bounds=&data.m_sequences[sequence].m_bounds;
    }
    if(!std::isfinite(bounds->m_radius))throw std::invalid_argument("Nonfinite original M2 hit radius");
    if(std::fabs(bounds->m_radius)<.00000023841858f)return {};
    auto result=std::shared_ptr<M2HitSnapshot>(new M2HitSnapshot);
    result->collision=collision;result->sse=(CM2Cache::s_processorFeatures&4)!=0;
    const C44Matrix transform=model.m_frameStamp==scene.m_frameStamp?model.m_matrixF4:model.m_worldTransform*scene.m_view;
    const auto& a=bounds->m_extent.t;const auto& b=bounds->m_extent.b;
    result->centre=C3Vector((a.x+b.x)*.5f,(a.y+b.y)*.5f,(a.z+b.z)*.5f)*transform;
    result->radiusSquared=bounds->m_radius*((transform.a2*transform.a2+transform.a1*transform.a1+transform.a0*transform.a0)*bounds->m_radius);
    if(!Finite(result->centre) || !std::isfinite(result->radiusSquared) || result->radiusSquared<0)
        throw std::invalid_argument("Nonfinite original M2 hit bounds");
    result->geometry=OwnGeometry(shared);const auto& source=*result->geometry;
    if(collision) {
        if(source.collisionIndices.size()%3)throw std::invalid_argument("Incomplete original M2 collision triangle");
        for(auto index:source.collisionIndices)if(index>=source.collisionPositions.size())throw std::invalid_argument("M2 collision index exceeds original positions");
        result->collisionTransform=transform;return result;
    }
    result->bones.resize(source.boneCount);
    if(source.boneCount)std::copy_n(model.m_boneMatrices,source.boneCount,result->bones.begin());
    for(const auto& matrix:result->bones) {
        float values[16];static_assert(sizeof(values)==sizeof(matrix));std::memcpy(values,&matrix,sizeof(values));
        for(float value:values)if(!std::isfinite(value))throw std::invalid_argument("Nonfinite evaluated original M2 hit pose");
    }
    const auto& skin=*shared.m_skinData;
    for(uint32_t i=0;i<skin.m_batches.m_count;++i) {
        const auto& batch=skin.m_batches[i];
        if(batch.m_materialLayer || (batch.m_flags&8))continue;
        if(batch.m_materialIndex>=data.m_materials.m_count || batch.m_skinSectionIndex>=source.sections.size())
            throw std::invalid_argument("M2 hit batch exceeds original material/section arrays");
        const auto& material=data.m_materials[batch.m_materialIndex];
        if((mode==2 && !material.m_blendMode) || (mode==1 && material.m_blendMode && !(material.m_flags&0x20)) || !model.m_skinSections[batch.m_skinSectionIndex])continue;
        float alpha=model.m_alpha;
        if(batch.m_colorIndex<data.m_colors.m_count) {
            if(!model.m_colors)throw std::invalid_argument("Missing original M2 hit colour pose");
            alpha*=model.m_colors[batch.m_colorIndex].m_alphaTrack.m_currentValue;
        }
        if(batch.m_textureCount) {
            if(batch.m_textureWeightComboIndex>=data.m_textureWeightCombos.m_count)throw std::invalid_argument("M2 hit weight combo exceeds source");
            const auto weight=data.m_textureWeightCombos[batch.m_textureWeightComboIndex];
            if(weight>=data.m_textureWeights.m_count || !model.m_textureWeights)throw std::invalid_argument("Missing original M2 hit texture weight pose");
            alpha*=model.m_textureWeights[weight].m_currentValue;
        }
        if(!std::isfinite(alpha))throw std::invalid_argument("Nonfinite original M2 hit alpha");
        if(alpha<=0)continue;
        const auto& section=source.sections[batch.m_skinSectionIndex];
        if(size_t(section.m_vertexStart)+section.m_vertexCount>source.mapping.size() || size_t(section.m_indexStart)+section.m_indexCount>source.indices.size() || section.m_indexCount%3)
            throw std::invalid_argument("M2 hit section exceeds original topology");
        for(size_t n=section.m_indexStart;n<size_t(section.m_indexStart)+section.m_indexCount;++n)
            if(source.indices[n]<section.m_vertexStart || size_t(source.indices[n])-section.m_vertexStart>=section.m_vertexCount)
                throw std::invalid_argument("M2 hit triangle exceeds original section");
        for(size_t n=section.m_vertexStart;n<size_t(section.m_vertexStart)+section.m_vertexCount;++n) {
            const auto index=source.mapping[n];if(index>=source.vertices.size())throw std::invalid_argument("M2 hit vertex mapping exceeds original positions");
            const auto& vertex=source.vertices[index];
            if(!Finite(vertex.m_position) || !Finite(vertex.m_normal))throw std::invalid_argument("Nonfinite original M2 hit vertex");
            const unsigned influences=section.m_boneInfluences==1?1:4;
            if(influences>1 && !vertex.m_weights.b[0])throw std::invalid_argument("Original blended M2 hit vertex has no first influence");
            for(unsigned k=0;k<influences && (!k || vertex.m_weights.b[k]);++k)
                if(vertex.m_indices.b[k]>=result->bones.size())throw std::invalid_argument("M2 hit bone index exceeds original pose");
        }
        result->sections.push_back(batch.m_skinSectionIndex);
    }
    return result;
}
uint64_t M2QueryHitSnapshots(CM2Scene& scene,const std::vector<M2HitSnapshotCandidate>& candidates,
    const C3Vector& start,const C3Vector& end,float& t,bool twoPass) {
    if(candidates.size()>UINT32_MAX)throw std::overflow_error("M2 hit candidate extent is not representable");
    if(!Finite(start) || !Finite(end) || !std::isfinite(t))throw std::invalid_argument("Nonfinite M2 hit query");
    if(t<.0000099999997f)return 0;
    const float dx=end.x-start.x,dy=end.y-start.y,dz=end.z-start.z;
    const float length=std::sqrt(dx*dx+dz*dz+dy*dy);if(length<.0000099999997f)return 0;
    if(!std::isfinite(length))throw std::invalid_argument("M2 hit ray extent overflow");
    const float inv=1.f/length;const C3Vector dir(dx*inv,dy*inv,inv*dz);
    std::vector<M2HitRec> records;std::vector<uint32_t> order,owners;size_t scratchCount=0;
    records.reserve(candidates.size());order.reserve(candidates.size());owners.reserve(candidates.size());
    for(size_t i=0;i<candidates.size();++i) {
        const auto& candidate=candidates[i];if(!candidate.pose)continue;
        if(!candidate.owner)throw std::invalid_argument("M2 hit snapshot has no original owner identity");
        const auto& pose=*candidate.pose;
        const auto delta=pose.centre-start;const float proj=dir.x*delta.x+dir.z*delta.z+dir.y*delta.y;
        const float px=dir.x*proj-delta.x,py=dir.y*proj-delta.y,pz=dir.z*proj-delta.z,dist=px*px+py*py+pz*pz;
        if(dist>pose.radiusSquared)continue;const float halfSq=pose.radiusSquared-dist,over=proj-length;
        if((proj<0 && proj*proj>halfSq) || (over>0 && over*over>halfSq))continue;
        const float half=std::sqrt(halfSq);
        records.push_back({nullptr,std::clamp(proj-half,0.f,length),std::clamp(proj+half,0.f,length),candidate.priority});
        owners.push_back(uint32_t(i));order.push_back(uint32_t(order.size()));
        if(pose.collision)scratchCount=std::max(scratchCount,pose.geometry->collisionPositions.size());
        else for(auto section:pose.sections)scratchCount=std::max(scratchCount,size_t(pose.geometry->sections[section].m_vertexCount));
    }
    if(records.empty())return 0;
    M2HeapSort(&CM2Scene::SortHitNear,order.data(),uint32_t(order.size()),records.data());
    Scratch scratch(scene,scratchCount);M2HitRec* best=nullptr;float tMax=t<1?t*length:length;
    const float d=start.z*dir.z+start.y*dir.y+start.x*dir.x;
    const auto project=[&](const C3Vector& p){
        if(!Finite(p))throw std::invalid_argument("Nonfinite evaluated original M2 hit position");
        const float dist=dir.z*p.z+dir.y*p.y+dir.x*p.x-d;return C3Vector(p.x-dir.x*dist,p.y-dir.y*dist,dist);
    };
    for(uint32_t pass=0;;) {
        for(auto i:order) {
            auto* rec=&records[i];
            if(pass) {if(best && (best->m_priority>rec->m_priority || (best->m_priority==rec->m_priority && tMax<=rec->m_tNear)))continue;}
            else if(tMax<=rec->m_tNear)break;
            const auto& pose=*candidates[owners[i]].pose;const auto& source=*pose.geometry;
            if(pose.collision) {
                for(size_t n=0;n<source.collisionPositions.size();++n)scratch.owned[n]=project(source.collisionPositions[n]*pose.collisionTransform);
                if(!source.collisionIndices.empty())best=scene.IntersectHitTestTriangles(source.collisionIndices.data(),source.collisionIndices.data()+source.collisionIndices.size(),0,start,pass,rec,tMax,best);
                continue;
            }
            for(auto index:pose.sections) {
                const auto& section=source.sections[index];C44Matrix matrix;uint32_t weights=0,indices=0;
                for(uint32_t n=0;n<section.m_vertexCount;++n) {
                    const auto& vertex=source.vertices[source.mapping[size_t(section.m_vertexStart)+n]];
                    if(section.m_boneInfluences==1)matrix=pose.bones[vertex.m_indices.b[0]];
                    else {
                        uint32_t w,b;std::memcpy(&w,&vertex.m_weights,4);std::memcpy(&b,&vertex.m_indices,4);
                        if(w!=weights || b!=indices) {
                            weights=w;indices=b;
                            if(pose.sse)M2BlendBoneMatricesSSE(pose.bones.data(),vertex.m_weights,vertex.m_indices,&matrix);
                            else M2BlendBoneMatrices(pose.bones.data(),vertex.m_weights,vertex.m_indices,&matrix);
                        }
                    }
                    C3Vector p=vertex.m_position*matrix;
                    if(pass) {
                        p.x=vertex.m_normal.z*matrix.c0+vertex.m_normal.y*matrix.b0+vertex.m_normal.x*matrix.a0+p.x;
                        p.y=vertex.m_normal.z*matrix.c1+vertex.m_normal.y*matrix.b1+vertex.m_normal.x*matrix.a1+p.y;
                        p.z=vertex.m_normal.z*matrix.c2+vertex.m_normal.y*matrix.b2+vertex.m_normal.x*matrix.a2+p.z;
                    }
                    scratch.owned[n]=project(p);
                }
                if(section.m_indexCount)best=scene.IntersectHitTestTriangles(source.indices.data()+section.m_indexStart,source.indices.data()+section.m_indexStart+section.m_indexCount,section.m_vertexStart,start,pass,rec,tMax,best);
            }
        }
        if(best || !twoPass || ++pass>=2)break;tMax=length;
    }
    if(!best)return 0;t=tMax/length;return candidates[owners[size_t(best-records.data())]].owner;
}
std::shared_ptr<const M2WorldCollisionSnapshot> M2CaptureWorldCollisionSnapshot(const CM2Model& model,const C44Matrix* sphereTransform) {
    if(!(model.m_fFlags&1) || model.m_frameStamp==UINT32_MAX)return {};
    if(!model.m_shared || !model.m_shared->m_data)throw std::invalid_argument("Missing original M2 world collision source");
    const auto& bounds=model.m_shared->m_data->m_collisionBounds;
    if(!std::isfinite(bounds.m_radius))throw std::invalid_argument("Nonfinite original M2 collision radius");
    if(std::fabs(bounds.m_radius)<.00000023841858f)return {};
    const auto& matrix=sphereTransform?*sphereTransform:model.m_matrixF4;
    ValidateMatrix(matrix);ValidateMatrix(model.m_worldTransform);
    auto result=std::shared_ptr<M2WorldCollisionSnapshot>(new M2WorldCollisionSnapshot);
    const auto& a=bounds.m_extent.t;const auto& b=bounds.m_extent.b;
    // Default physics uses ORIGINAL matrixF4, including stale/attached models.
    // An explicit capture models a preceding view query's original update.
    result->centre=C3Vector((a.x+b.x)*.5f,(a.y+b.y)*.5f,.5f*(a.z+b.z))*matrix;
    const auto& m=matrix;
    result->radiusSquared=bounds.m_radius*((m.a2*m.a2+m.a1*m.a1+m.a0*m.a0)*bounds.m_radius);
    if(!Finite(result->centre) || !std::isfinite(result->radiusSquared) || result->radiusSquared<0)throw std::invalid_argument("Invalid original M2 world collision sphere");
    const float determinant=model.m_worldTransform.Determinant();
    if(!std::isfinite(determinant) || determinant==0)throw std::invalid_argument("Singular original M2 world collision transform");
    result->inverse=model.m_worldTransform.Inverse(determinant);ValidateMatrix(result->inverse);
    result->geometry=OwnGeometry(*model.m_shared);const auto& source=*result->geometry;
    if(source.collisionIndices.size()%3)throw std::invalid_argument("Incomplete original M2 world collision triangle");
    for(auto index:source.collisionIndices)if(index>=source.collisionPositions.size())throw std::invalid_argument("M2 world collision index exceeds original source");
    for(const auto& p:source.collisionPositions)if(!Finite(p))throw std::invalid_argument("Nonfinite original M2 world collision position");
    return result;
}
uint64_t M2QueryWorldCollisionSnapshots(const std::vector<M2WorldCollisionCandidate>& candidates,const C44Matrix& view,
    const C3Vector& start,const C3Vector& end,float& t) {
    if(candidates.size()>UINT32_MAX)throw std::overflow_error("M2 world collision candidate extent is not representable");
    if(!Finite(start)||!Finite(end)||!std::isfinite(t))throw std::invalid_argument("Nonfinite M2 world collision query");ValidateMatrix(view);
    if(t<.0000099999997f)return 0;
    const auto delta=end-start;const float length=std::sqrt(delta.x*delta.x+delta.z*delta.z+delta.y*delta.y);
    if(!std::isfinite(length))throw std::invalid_argument("M2 world collision ray extent overflow");if(length<.0000099999997f)return 0;
    const float inv=1.f/length;const C3Vector dir(delta.x*inv,delta.y*inv,inv*delta.z),viewStart=start*view;const C33Matrix rotation(view);
    const C3Vector viewDir(rotation.a0*dir.x+rotation.b0*dir.y+rotation.c0*dir.z,rotation.c1*dir.z+rotation.b1*dir.y+rotation.a1*dir.x,dir.y*rotation.b2+dir.z*rotation.c2+dir.x*rotation.a2);
    const float viewLength=std::sqrt(view.a2*view.a2+view.a1*view.a1+view.a0*view.a0)*length;
    if(!Finite(viewStart)||!Finite(viewDir)||!std::isfinite(viewLength))throw std::invalid_argument("M2 world collision view extent overflow");
    std::vector<M2HitRec> records;std::vector<uint32_t> order,owners;
    for(size_t i=0;i<candidates.size();++i) {
        const auto& c=candidates[i];if(!c.pose)continue;if(!c.owner)throw std::invalid_argument("Missing M2 world collision owner");const auto& pose=*c.pose;
        const auto d=pose.centre-viewStart;const float proj=viewDir.x*d.x+viewDir.z*d.z+viewDir.y*d.y;
        const float px=viewDir.x*proj-d.x,py=viewDir.y*proj-d.y,pz=viewDir.z*proj-d.z,dist=px*px+py*py+pz*pz;
        if(!std::isfinite(proj)||!std::isfinite(dist))throw std::invalid_argument("M2 world collision sphere query overflow");
        if(dist>pose.radiusSquared)continue;const float halfSq=pose.radiusSquared-dist,over=proj-viewLength;
        if((proj<0 && proj*proj>halfSq)||(over>0 && over*over>halfSq))continue;const float half=std::sqrt(halfSq);
        records.push_back({nullptr,std::clamp(proj-half,0.f,viewLength),std::clamp(proj+half,0.f,viewLength),c.priority});
        owners.push_back(uint32_t(i));order.push_back(uint32_t(order.size()));
    }
    if(order.empty())return 0;M2HeapSort(&CM2Scene::SortHitNear,order.data(),uint32_t(order.size()),records.data());
    float maximum=t<1?length*t:length;uint64_t best=0;
    for(auto index:order) {
        if(maximum<=records[index].m_tNear)break;const auto& candidate=candidates[owners[index]];const auto& pose=*candidate.pose;
        const auto localStart=start*pose.inverse,localEnd=end*pose.inverse;const auto d=localEnd-localStart;
        const float localLength=std::sqrt(d.y*d.y+d.z*d.z+d.x*d.x);
        if(!Finite(localStart)||!Finite(localEnd)||!std::isfinite(localLength)||!localLength)throw std::invalid_argument("Invalid M2 world collision local ray");
        const float localInv=1.f/localLength;C3Ray ray;ray.origin=localStart;ray.dir=C3Vector(d.x*localInv,d.y*localInv,localInv*d.z);
        const auto& source=*pose.geometry;
        for(size_t n=0;n<source.collisionIndices.size();n+=3) {
            float hit;
            if(NTempest::Intersect(ray,source.collisionPositions.data(),source.collisionIndices.data()+n,&hit,nullptr,.000001f) && hit>=0) {
                const float distance=hit*localInv*length;
                if(!std::isfinite(distance))throw std::invalid_argument("M2 world collision hit distance overflow");
                if(maximum>=distance){maximum=distance;best=candidate.owner;}
            }
        }
    }
    if(best)t=maximum/length;return best;
}
std::shared_ptr<const M2HitSceneSnapshot> M2HitSceneSnapshot::Capture(CM2Scene& scene,const std::vector<M2HitSnapshotRoot>& input,uint32_t mode) {
    if(input.size()>UINT32_MAX)throw std::overflow_error("M2 hit root extent is not representable");
    auto result=std::shared_ptr<M2HitSceneSnapshot>(new M2HitSceneSnapshot);
    std::vector<const CM2Model*> sources;
    std::unordered_map<const CM2Model*,uint32_t> indices;
    const auto intern=[&](const CM2Model* model) {
        if(!model)throw std::invalid_argument("Missing original M2 hit root/model");
        const auto found=indices.find(model);if(found!=indices.end())return found->second;
        if(sources.size()>=UINT32_MAX)throw std::overflow_error("M2 attachment graph index exhausted");
        const auto index=uint32_t(sources.size());indices.emplace(model,index);sources.push_back(model);result->nodes.emplace_back();return index;
    };
    for(const auto& root:input) {
        if(!root.owner)throw std::invalid_argument("Missing original M2 hit root owner");
        result->roots.push_back({intern(root.model),root.owner});
    }
    for(size_t i=0;i<sources.size();++i) {
        const auto* model=sources[i];result->nodes[i].loaded=(model->m_fFlags&1)!=0;
        result->nodes[i].pose=M2CaptureHitSnapshot(scene,*model,mode);
        std::unordered_set<const CM2Model*> siblings;
        for(auto* child=model->m_attachList;child;child=child->m_attachNext) {
            if(!siblings.insert(child).second || child->m_attachParent!=model)
                throw std::invalid_argument("Invalid original M2 attachment graph");
            const auto index=intern(child);result->nodes[i].children.push_back(index);
        }
    }
    // Parent IDs never borrow an actor. A selected root supplies its own owner,
    // so ancestors outside the captured root subtrees are not needed to pick it.
    for(size_t i=0;i<sources.size();++i) {
        const auto parent=indices.find(sources[i]->m_attachParent);
        if(parent!=indices.end())result->nodes[i].parent=parent->second;
    }
    std::vector<uint8_t> colour(sources.size());
    for(uint32_t i=0;i<sources.size();++i) {
        auto node=i;
        while(node!=UINT32_MAX && !colour[node]){colour[node]=1;node=result->nodes[node].parent;}
        if(node!=UINT32_MAX && colour[node]==1)throw std::invalid_argument("Cyclic original M2 attachment graph");
        node=i;while(node!=UINT32_MAX && colour[node]==1){colour[node]=2;node=result->nodes[node].parent;}
    }
    return result;
}
uint64_t M2HitSceneSnapshot::RootOwner(uint32_t root) const {
    if(root>=roots.size())throw std::out_of_range("M2 hit root selection exceeds captured generation");
    return roots[root].owner;
}
uint64_t M2HitSceneSnapshot::Query(CM2Scene& scene,const std::vector<M2HitRootSelection>& selected,
    const C3Vector& start,const C3Vector& end,float& t,bool twoPass) const {
    struct Work {uint32_t node;uint64_t owner;};
    std::vector<uint8_t> included(nodes.size());std::vector<uint64_t> owners(nodes.size());
    std::vector<uint32_t> priorities(nodes.size()),order;std::vector<Work> stack;
    for(const auto& selection:selected) {
        if(selection.root>=roots.size())throw std::out_of_range("M2 hit root selection exceeds captured generation");
        const auto& root=roots[selection.root];stack.push_back({root.node,root.owner});
        while(!stack.empty()) {
            const auto work=stack.back();stack.pop_back();const auto& node=nodes[work.node];
            if(node.loaded) {
                if(!included[work.node]){included[work.node]=1;order.push_back(work.node);}
                // As in AddModelToHitTest, an explicit root can update an
                // already-inserted attachment's owner/priority without moving it.
                owners[work.node]=work.owner;priorities[work.node]=selection.priority;
            }
            for(auto child=node.children.rbegin();child!=node.children.rend();++child)
                if(!included[*child])stack.push_back({*child,0});
        }
    }
    std::vector<M2HitSnapshotCandidate> candidates;candidates.reserve(order.size());
    // The original hit list pushes newly visited models at its head.
    for(auto it=order.rbegin();it!=order.rend();++it) {
        const auto index=*it;if(!nodes[index].pose)continue;auto owner=index;
        while(owner!=UINT32_MAX && !owners[owner])owner=nodes[owner].parent;
        if(owner!=UINT32_MAX)candidates.push_back({nodes[index].pose,owners[owner],priorities[index]});
    }
    return M2QueryHitSnapshots(scene,candidates,start,end,t,twoPass);
}
