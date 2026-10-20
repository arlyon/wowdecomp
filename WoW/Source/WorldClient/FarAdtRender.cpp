#include "FarAdtRender.h"
#include "FarAdtService.h"
#include "FarAdtPrepared.h"
#include "Map.h"
#include "MapLowDetail.h"
#include "MapRenderChunk.h"
#include "MapChunk.h"
#include "World.h"
#include "DayNight__DNInfo.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/FarResources.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Light.h"
#include <stdexcept>
#include "Engine/Source/Storm/Source/Sha1.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

namespace {
using Lease=std::shared_ptr<GxFarResourceLease>;
using TextureKey=std::tuple<std::array<uint8_t,20>,uint32_t,uint32_t,FarAdt::AlbedoEncoding>;
struct Texture {std::weak_ptr<GxFarResourceLease> lease;std::shared_ptr<const void> charge;};
struct Entry {
    std::shared_ptr<const void> charge;
    FarAdt::ReadyPayload ticket;
    Lease geometry,blend;
    std::vector<Lease> materials;
    std::array<FarAdt::PreparedChunk,256> chunks;
    std::array<CAaSphere,256> spheres;
    std::array<C3Vector,256> origins;
    uint32_t mapFlags=0;
    bool authoredEmpty=false;
};
struct State {
    std::shared_ptr<const void> charge;
    std::map<unsigned,std::unique_ptr<Entry>> tiles;
    std::map<TextureKey,Texture> textures;
    std::shared_ptr<const FarAdt::PreparedTile> pending;
    uint32_t uploadFrame=UINT32_MAX;
};
std::unique_ptr<State> state;
unsigned TileIndex(FarAdt::Key key) {return unsigned(key.y)*64+key.x;}
void Release(Entry& e) {
    if(e.geometry) e.geometry->Withdraw();
    if(e.blend) e.blend->Withdraw();
    // Shared layer images withdraw only when their last tile/backend reader dies.
    FarAdtRelease(e.ticket);
}
bool Current(const Entry& e) {
    const auto* producer=FarAdtGetProducer();
    return producer && producer->Current(e.ticket) && (e.authoredEmpty || (e.geometry && e.blend &&
        e.geometry->Status()==GxFarResourceStatus::Resident && e.blend->Status()==GxFarResourceStatus::Resident &&
        std::all_of(e.materials.begin(),e.materials.end(),[](const Lease& p){return p && p->Status()==GxFarResourceStatus::Resident;})));
}
GxFarTextureFormat Encoding(FarAdt::AlbedoEncoding encoding) {
    switch(encoding) {
        case FarAdt::AlbedoEncoding::BC1:return GxFarTextureFormat::Bc1;
        case FarAdt::AlbedoEncoding::BC2:return GxFarTextureFormat::Bc2;
        case FarAdt::AlbedoEncoding::BC3:return GxFarTextureFormat::Bc3;
        case FarAdt::AlbedoEncoding::RGBA8:return GxFarTextureFormat::Rgba8;
    }
    throw std::invalid_argument("Unknown prepared FAR encoding");
}
Lease Admit(CGxDevice& gx,const GxFarResourceRequest& request) {
    const auto result=gx.SceneCreateFarResources(request);
    auto& census=CMap::s_lowDetail.m_census;
    switch(result.result) {
        case GxFarResourceResult::Budget:++census.preparedBudget;break;
        case GxFarResourceResult::AllocationFailure:++census.preparedAllocationFailure;break;
        case GxFarResourceResult::Unsupported:++census.preparedUnsupported;break;
        case GxFarResourceResult::Unavailable:++census.preparedUnavailable;break;
        case GxFarResourceResult::Invalid:throw std::logic_error("Prepared FAR resource violated its upload contract");
        case GxFarResourceResult::Ready:break;
    }
    return result.result==GxFarResourceResult::Ready?result.lease:nullptr;
}
Lease Material(CGxDevice& gx,const FarAdt::PreparedTile& tile,const FarAdt::PreparedTexture& texture) {
    if(texture.offset>tile.materialBytes || texture.bytes>tile.materialBytes-texture.offset) return {};
    const auto* bytes=tile.albedo.data()+texture.offset;
    std::array<uint8_t,20> digest{};Sha1 hash;hash.Initialize();hash.Append(bytes,texture.bytes);hash.Finalize(digest.data());
    TextureKey key{digest,texture.width,texture.height,texture.encoding};
    const auto found=state->textures.find(key);
    if(found!=state->textures.end()) {
        auto resident=found->second.lease.lock();
        if(resident && resident->Status()==GxFarResourceStatus::Resident) return resident;
        state->textures.erase(found);
    }
    auto charge=GxFar::ReserveFarCpuBytes(sizeof(Texture)+sizeof(TextureKey)+128,GxFar::FarCpuClass::ResourceMetadata);
    if(!charge) return {};
    GxFarResourceRequest request;request.width=texture.width;request.height=texture.height;
    request.format=Encoding(texture.encoding);request.wrapU=request.wrapV=true;request.mips={{bytes,texture.bytes}};
    auto lease=Admit(gx,request);
    if(lease) state->textures.emplace(std::move(key),Texture{lease,std::move(charge)});
    return lease;
}
void Upload(CGxDevice& gx) {
    if(state->uploadFrame==CWorld::frameCnt) return;
    state->uploadFrame=CWorld::frameCnt;
    if(!state->pending) state->pending=FarAdtConsumePrepared();
    if(!state->pending) return;
    const auto& p=*state->pending;
    if(!FarAdtPreparedCurrent(p) || !CMap::s_lowDetail.GetCensus().admittedIds.test(TileIndex(p.key))) {
        FarAdtReleasePrepared(state->pending);return;
    }
    auto charge=GxFar::ReserveFarCpuBytes(sizeof(Entry)+p.textureCount*sizeof(Lease)+128,GxFar::FarCpuClass::ResourceMetadata);
    if(!charge) return;
    auto entry=std::make_unique<Entry>();entry->charge=std::move(charge);entry->mapFlags=p.mapFlags;entry->chunks=p.chunks;
    entry->authoredEmpty=p.indexCount==0;
    if(entry->authoredEmpty) {
        entry->ticket={p.key,p.ticket,{},p.map,p.generation};
        state->tiles.emplace(TileIndex(p.key),std::move(entry));state->pending.reset();return;
    }
    GxFarResourceRequest request;request.vertices=p.vertices.data();request.vertexStride=sizeof(FarAdt::PreparedVertex);
    request.vertexCount=p.vertices.size();request.indices=p.indices.data();request.indexCount=p.indexCount;
    entry->geometry=Admit(gx,request);if(!entry->geometry) return;
    request={};request.width=request.height=FarAdt::BlendAtlasSide;request.mips={{p.blend.data(),p.blend.size()}};
    entry->blend=Admit(gx,request);if(!entry->blend) return;
    entry->materials.reserve(p.textureCount);
    for(unsigned i=0;i<p.textureCount;++i) {
        auto material=Material(gx,p,p.textures[i]);if(!material) return;
        entry->materials.push_back(std::move(material));
    }
    for(unsigned ci=0;ci<256;++ci) {
        const auto& first=p.vertices[ci*145];entry->origins[ci]={first.position[0],first.position[1],first.position[2]};
        auto& sphere=entry->spheres[ci];C3Vector low=entry->origins[ci],high=low;
        for(unsigned v=0;v<145;++v) {
            const auto& position=p.vertices[ci*145+v].position;
            const C3Vector point(position[0],position[1],position[2]);
            low.Minimize(point);high.Maximize(point);
        }
        sphere.c=(low+high)*.5f;sphere.r=std::sqrt((high-low).SquaredMag())*.5f;
    }
    entry->ticket={p.key,p.ticket,{},p.map,p.generation};
    const auto index=TileIndex(p.key);auto old=state->tiles.find(index);
    if(old!=state->tiles.end()) {Release(*old->second);state->tiles.erase(old);}
    state->tiles.emplace(index,std::move(entry));
    // Preserve the producer's transferred ticket, but release all CPU geometry,
    // decoded blend data and material bytes after their immutable GPU upload.
    state->pending.reset();
}
struct Constants {
    float saved[2][1024];
    CMapRenderChunk::SVertexShaderConstants terrain=CMapRenderChunk::s_vertexShaderConsts;
    Constants() {for(unsigned i=0;i<2;++i)std::memcpy(saved[i],CGxDevice::s_shadowConstants[i].constants,sizeof(saved[i]));}
    ~Constants() {
        CMapRenderChunk::s_vertexShaderConsts=terrain;
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex,0,saved[1],256);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel,0,saved[0],256);
        g_theGxDevicePtr->SceneSetMotionOwner(nullptr,false);
    }
};
unsigned Lod(const Entry& entry,unsigned ci,const CGxDevice& gx) {
    const auto& sphere=entry.spheres[ci];
    const float distance=std::max(1.f,std::sqrt((sphere.c-CWorldScene::s_activeWorldView).SquaredMag())-sphere.r);
    const float focal=std::abs(gx.m_projNative.b1)*gx.m_format.m_size.y*.5f;
    unsigned result=0;
    for(unsigned lod=1;lod<5;++lod) if(entry.chunks[ci].lodError[lod]*focal/distance<=1.f)result=lod;
    return result;
}
}
void FarAdtRenderMaintain() {
    if(!state) return;
    for(auto it=state->tiles.begin();it!=state->tiles.end();) {
        if(!Current(*it->second) || !CMap::s_lowDetail.GetCensus().admittedIds.test(it->first)) {
            Release(*it->second);it=state->tiles.erase(it);
        } else ++it;
    }
    for(auto it=state->textures.begin();it!=state->textures.end();) {
        auto lease=it->second.lease.lock();
        if(!lease || lease->Status()!=GxFarResourceStatus::Resident) it=state->textures.erase(it);else ++it;
    }
}
void FarAdtRenderReset() {
    if(!state) return;
    for(auto& item:state->tiles) Release(*item.second);
    FarAdtReleasePrepared(state->pending);state.reset();
}
bool FarAdtRenderTile(CMapAreaLow* area) {
    auto& gx=*g_theGxDevicePtr;
    if(!gx.SceneFarResourcesSupported() || !CMap::enableTerrainShaderVertex || !CMap::gTerrainPixelShadersValid) return false;
    try {
        if(!state) {
            auto charge=GxFar::ReserveFarCpuBytes(sizeof(State)+256,GxFar::FarCpuClass::ResourceMetadata);
            if(!charge) {++CMap::s_lowDetail.m_census.preparedBudget;return false;}
            state=std::make_unique<State>();state->charge=std::move(charge);
        }
        Upload(gx);
    } catch(const std::bad_alloc&) {return false;}
    const auto found=state->tiles.find(unsigned(area->m_index.y)*64+unsigned(area->m_index.x));
    if(found==state->tiles.end() || !Current(*found->second)) return false;
    auto& entry=*found->second;
    if(entry.authoredEmpty) return true;
    for(const auto& chunk:entry.chunks) {
        const bool unlit=std::any_of(chunk.layers.begin(),chunk.layers.begin()+chunk.layerCount,[](const FarAdt::Layer& l){return l.flags&0x80;});
        const bool reflection=std::any_of(chunk.layers.begin(),chunk.layers.begin()+chunk.layerCount,[](const FarAdt::Layer& l){return l.flags&0x400;});
        auto* vs=CMapRenderChunk::GetVertexShader(0,chunk.layerCount,CMap::enableSpecularTerrain,1,reflection,0);
        auto* localVs=CMapRenderChunk::GetVertexShader(1,chunk.layerCount,CMap::enableSpecularTerrain,1,reflection,0);
        auto* ps=CMap::GetTerrainPixelShader((entry.mapFlags>>2)&1,chunk.layerCount,0,reflection,unlit);
        if(!vs || !localVs || !ps || !vs->Valid() || !localVs->Valid() || !ps->Valid()) {++CMap::s_lowDetail.m_census.preparedUnsupported;return false;}
    }
    Constants restore;
    C44Matrix view;GxXformView(view);
    const auto& world=gx.m_xforms[GxXform_World].m_mtx[gx.m_xforms[GxXform_World].m_level];
    CMapRenderChunk::InitializeVertexShaderConstants(world,view);
    const auto initial=CMapRenderChunk::s_vertexShaderConsts;
    gx.RsSet(GxRs_BlendingMode,GxBlend_Opaque);gx.RsSet(GxRs_AlphaRef,0);gx.RsSet(GxRs_ColorWrite,15);
    gx.RsSet(GxRs_DepthTest,1);gx.RsSet(GxRs_DepthWrite,1);gx.RsSet(GxRs_DepthFunc,0);gx.RsSet(GxRs_Culling,0);
    const CGxVertexAttrib attrs[]={{GxVA_Position,4,0,sizeof(FarAdt::PreparedVertex)},
        {GxVA_Normal,4,12,sizeof(FarAdt::PreparedVertex)},{GxVA_Color0,0,24,sizeof(FarAdt::PreparedVertex)}};
    gx.PrimVertexFormat(entry.geometry->VertexBuffer(),attrs,3);
    gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Normal)|(1u<<GxVA_Color0));gx.PrimIndexPtr(entry.geometry->IndexBuffer());
    gx.SceneSetMotionOwner(&entry,true);
    const auto& fog=DayNight::GetInfo()->m_fog.m_color;
    const float fogColour[]={fog.r/255.f,fog.g/255.f,fog.b/255.f,0};gx.ShaderConstantsSet(GxSh_Pixel,2,fogColour,1);
    uint32_t triangles=0,chunks=0;
    for(unsigned ci=0;ci<256;++ci) {
        const auto& chunk=entry.chunks[ci];const auto lod=Lod(entry,ci,gx);
        if(!chunk.indexCount[lod])continue;
        auto constants=initial;
        constants.m_position.x=entry.origins[ci].x;constants.m_position.y=entry.origins[ci].y;constants.m_position.z=entry.origins[ci].z;
        float lit[4]={1,1,1,1};bool unlit=false,reflection=false;
        for(unsigned layer=0;layer<chunk.layerCount;++layer) {
            const auto& data=chunk.layers[layer];
            gx.RsSet(static_cast<EGxRenderState>(GxRs_Texture0+layer),entry.materials[data.textureId]->Texture());
            constants.m_texScale[layer+1].x=constants.m_texScale[layer+1].y=-CMapChunk::s_geoToTex;
            const auto offset=FarAdt::LayerUvOffset(data.flags,{World::texVect[data.flags&7].x,World::texVect[data.flags&7].y});
            constants.m_texOffset[layer].x=offset[0];constants.m_texOffset[layer].y=offset[1];
            if(data.flags&0x80) {lit[layer]=0;unlit=true;}
            reflection=reflection || (data.flags&0x400);
        }
        const float atlasScale=float(FarAdt::BlendChunkSide)/FarAdt::BlendAtlasSide;
        C4Vector atlasOffset;
        atlasOffset.x=float(ci/16*FarAdt::BlendChunkStride+1)/FarAdt::BlendAtlasSide;
        atlasOffset.y=float(ci%16*FarAdt::BlendChunkStride+1)/FarAdt::BlendAtlasSide;atlasOffset.z=atlasOffset.w=0;
        if(chunk.layerCount<4)constants.m_texOffset[chunk.layerCount]=atlasOffset;
        else constants.m_texScale[0]=atlasOffset; // original c17 aliases the fifth UV offset
        constants.m_texScale[chunk.layerCount+1].x=constants.m_texScale[chunk.layerCount+1].y=-CMapChunk::s_geoToTex*.125f*atlasScale;
        gx.RsSet(static_cast<EGxRenderState>(GxRs_Texture0+chunk.layerCount),entry.blend->Texture());
        CM2Lighting lighting(entry.spheres[ci]);
        if(World::s_m2Scene) World::s_m2Scene->SelectLights(&lighting);
        bool pointLights=false;
        for(unsigned i=0;i<3;++i) {
            C3Vector position;auto& light=constants.m_pointLights[i];
            if(lighting.GetPointLight(i,position,light.m_color,light.m_attenuation)) {
                light.m_position=position-CWorldScene::s_activeWorldView;light.m_positionW=1;pointLights=true;
            }
        }
        gx.RsSet(GxRs_VertexShader,CMapRenderChunk::GetVertexShader(pointLights,chunk.layerCount,CMap::enableSpecularTerrain,1,reflection,0));
        gx.RsSet(GxRs_PixelShader,CMap::GetTerrainPixelShader((entry.mapFlags>>2)&1,chunk.layerCount,0,reflection,unlit));
        gx.ShaderConstantsSet(GxSh_Vertex,0,reinterpret_cast<const float*>(&constants),sizeof(constants)/sizeof(C4Vector));
        gx.ShaderConstantsSet(GxSh_Pixel,1,lit,1);
        CGxBatch batch{};batch.m_primType=GxPrim_Triangles;batch.m_start=chunk.firstIndex[lod];batch.m_count=chunk.indexCount[lod];
        batch.m_minIndex=uint16_t(ci*145);batch.m_maxIndex=uint16_t(ci*145+144);gx.Draw(&batch,1);
        auto& census=CMap::s_lowDetail.m_census;
        const float distance=std::max(0.f,std::sqrt((entry.spheres[ci].c-CWorldScene::s_activeWorldView).SquaredMag())-entry.spheres[ci].r);
        census.minPreparedDistance=census.minPreparedDistance>0?std::min(census.minPreparedDistance,distance):distance;
        triangles+=batch.m_count/3;++chunks;
    }
    auto& low=CMap::s_lowDetail;low.RecordSubmission(area,triangles,true);
    ++low.m_census.preparedTiles;low.m_census.preparedChunks+=chunks;low.m_census.preparedTriangles+=triangles;
    const auto delta=area->m_sphere.c-low.m_census.camera;
    low.m_census.maxPreparedDistance=std::max(low.m_census.maxPreparedDistance,std::sqrt(delta.SquaredMag()));
    return true;
}
