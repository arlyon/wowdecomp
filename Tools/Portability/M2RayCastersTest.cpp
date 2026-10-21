// Restored from the preserved M2 ray fixture: original profile permutation,
// palette deformation and source coverage cases, adapted to M2ShadowMesh.
// Only loading/scheduling/resource handles are stubbed; production collection
// and geometry/opacity decisions are linked unchanged. No texture recapture.
#include "Engine/Source/Model2/M2ShadowMesh.h"
#include "Engine/Source/Model2/M2Cache.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxTex.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
int schedules=0,initialisations=0,textureLookups=0,leases=0;
CM2Lighting::CM2Lighting() {}
CM2Light::CM2Light() {}
CM2Light::~CM2Light()=default;
CM2Model::CM2Model(){m_scene=nullptr;m_shared=nullptr;m_animatePrev=nullptr;m_animateNext=nullptr;m_attachParent=nullptr;m_optGeo=nullptr;m_frameStamp=0;m_fFlags=1;m_flags=0;m_sceneNext=nullptr;}
CM2Model::~CM2Model()=default;
int32_t CM2Model::InitializeLoaded(){++initialisations;m_fFlags|=1;return 1;}
void CM2Model::SetAnimating(int32_t enabled){assert(enabled&&(m_fFlags&1));++schedules;m_animatePrev=&m_scene->m_animateList;m_animateNext=m_scene->m_animateList;m_scene->m_animateList=this;}
CM2Cache::CM2Cache():m_threadStart(0,0),m_threadDone(0,0){m_flags=8;}
CM2Cache::~CM2Cache()=default;
CM2Shared::CM2Shared(CM2Cache* c){m_cache=c;m_flags=3;}
CM2Shared::~CM2Shared()=default;
CM2Scene::CM2Scene(CM2Cache* c){m_cache=c;m_frameStamp=5;m_modelList=m_animateList=nullptr;}
CM2Scene::~CM2Scene()=default;
template<>M2ModelTrack<float>::M2ModelTrack(){m_currentValue=1;}
CHandleObject::CHandleObject():m_refcount(0){}
CHandleObject::~CHandleObject()=default;
const char* CHandleObject::GetObjectName(){return "fixture";}
HASHKEY_TEXTUREFILE::HASHKEY_TEXTUREFILE():m_filename(nullptr),m_anyTexFlags(0){}
CTexture::CTexture(){m_flags=0;m_gxTex=nullptr;m_asyncObject=nullptr;m_atlas=nullptr;}
CTexture::~CTexture()=default;
const char* CTexture::GetObjectName(){return "fixture";}
CGxTexFlags::CGxTexFlags(EGxTexFilter f,uint32_t u,uint32_t v,uint32_t track,uint32_t mip,uint32_t rt,uint32_t aniso,uint32_t b8,uint32_t b14,uint32_t b15){std::memset(this,0,sizeof(*this));m_filter=f;m_wrapU=u;m_wrapV=v;m_forceMipTracking=track;m_generateMipMaps=mip;m_renderTarget=rt;m_maxAnisotropy=aniso;m_bit8=b8;m_bit14=b14;m_bit15=b15;}
CGxTex::CGxTex(EGxTexTarget t,uint32_t w,uint32_t h,uint32_t d,EGxTexFormat f,EGxTexFormat data,CGxTexFlags flags,void* user,GxTexUserFunc func,const char*){m_target=t;m_width=w;m_height=h;m_depth=d;m_format=f;m_dataFormat=data;m_flags=flags;m_userArg=user;m_userFunc=func;}
CGxTex::~CGxTex()=default;
CGxTex* TextureGetGxTex(CTexture* t,int32_t wait,CStatus*){assert(!wait);++textureLookups;return t->m_gxTex;}
HOBJECT HandleDuplicate(HOBJECT h){++leases;return h;}
void HandleClose(HOBJECT){assert(leases>0);--leases;}
template<class T,size_t N>void Bind(M2Array<T>& a,T(&b)[N]){a.m_count=N;a.Set(b);}
void Near(float a,float b){assert(std::isfinite(a)&&std::fabs(a-b)<.0003f);}
struct Fixture{
 CM2Cache cache;CM2Scene scene{&cache};CM2Shared shared{&cache};CM2Model model;
 M2Data data{};M2SkinProfile skin{};M2Vertex vertices[3]{};M2CompBone bones[4]{};
 uint16_t permutation[3]={2,0,1},indices[3]={2,0,1},combos[4]={3,1,2,0};ubyte4 locals[3]{};
 M2SkinSection section[1]{};M2Batch batch[1]{};M2Material materials[1]{};M2Texture textureData[1]{};M2TextureWeight weightData[1]{};
 uint16_t texCombo[1]={0},coordCombo[1]={0},weightCombo[1]={0},transformCombo[1]={0xffff};uint32_t visible[1]={1};
 M2ModelTrack<float> weights[1];C44Matrix palette[4];CTexture texture;CTexture* bindings[1]={&texture};
 CGxTex native{GxTex_2d,1,1,1,GxTex_Argb8888,GxTex_Argb8888,CGxTexFlags{},nullptr,nullptr,"fixture"};
 Fixture(unsigned influences=1){
  Bind(data.m_vertices,vertices);Bind(data.m_bones,bones);Bind(data.m_boneCombos,combos);Bind(data.m_materials,materials);Bind(data.m_textures,textureData);Bind(data.m_textureWeights,weightData);
  Bind(data.m_textureCombos,texCombo);Bind(data.m_textureCoordCombos,coordCombo);Bind(data.m_textureWeightCombos,weightCombo);Bind(data.m_textureTransformCombos,transformCombo);
  Bind(skin.m_vertices,permutation);Bind(skin.m_indices,indices);Bind(skin.m_bones,locals);Bind(skin.m_skinSections,section);Bind(skin.m_batches,batch);
  section[0].m_vertexCount=3;section[0].m_indexCount=3;section[0].m_boneCount=4;section[0].m_boneInfluences=influences;batch[0].m_colorIndex=0xffff;batch[0].m_textureCount=1;
  shared.m_data=&data;shared.m_skinData=&skin;model.m_shared=&shared;model.m_scene=&scene;scene.m_modelList=&model;model.m_boneMatrices=palette;model.m_skinSections=visible;model.m_textureWeights=weights;model.m_textures=bindings;model.m_textureMatrices=nullptr;model.m_colors=nullptr;model.m_alpha=1;texture.m_gxTex=&native;
  for(unsigned j=0;j<3;++j){vertices[j].m_position=C3Vector(float(j),0,1);vertices[j].m_normal=C3Vector(0,0,1);vertices[j].m_texcoord[0]=C2Vector(.25f,.75f);vertices[j].m_indices={{99,99,99,99}};vertices[j].m_weights=influences==1?ubyte4{{255,0,0,0}}:ubyte4{{128,127,0,0}};locals[j]={{0,1,2,3}};}
  Pose(0);
 }
 void Pose(float camera,float motion=0){scene.m_viewInv=C44Matrix();scene.m_viewInv.d0=camera;for(unsigned i=0;i<4;++i){palette[i]=C44Matrix();palette[i].d0=motion+i*2-camera;}model.m_frameStamp=scene.m_frameStamp;}
 void Software(){M2Vertex copy[3];for(unsigned j=0;j<3;++j){copy[j]=vertices[permutation[j]];for(unsigned k=0;k<4;++k)copy[j].m_indices.b[k]=combos[locals[j].b[k]];}for(unsigned j=0;j<3;++j){vertices[j]=copy[j];permutation[j]=j;}cache.m_flags=0;}
};
int main(){
 using namespace GxShadow;
 for(unsigned influences:{1u,2u,4u}){
  Fixture f(influences);Mesh hardware,software;assert(BuildM2ShadowMesh(f.model,f.scene,0,hardware)==FrameResult::Ready);assert(hardware.positions.size()==3&&hardware.indices==std::vector<uint32_t>({2,0,1}));
  M2ModelOptGeo optimized{};optimized.m_batches=f.batch;optimized.m_batchCount=1;optimized.m_skinSections=f.section;optimized.m_skinSectionCount=1;
  f.model.m_optGeo=&optimized;Mesh optimizedSnapshot;
  assert(BuildM2ShadowMesh(f.model,f.scene,0,optimizedSnapshot)==FrameResult::Ready);
  assert(optimizedSnapshot.indices==hardware.indices);for(unsigned j=0;j<3;++j)Near(optimizedSnapshot.positions[j].xyz[0],hardware.positions[j].xyz[0]);
  f.visible[0]=0;assert(BuildM2ShadowMesh(f.model,f.scene,0,optimizedSnapshot)==FrameResult::Ready&&optimizedSnapshot.indices.empty());f.visible[0]=1;
  const auto original=hardware.positions;f.Pose(100,3);Mesh moved;assert(BuildM2ShadowMesh(f.model,f.scene,0,moved)==FrameResult::Ready);for(unsigned j=0;j<3;++j)Near(moved.positions[j].xyz[0],original[j].xyz[0]+3);
  f.Software();assert(BuildM2ShadowMesh(f.model,f.scene,0,software)==FrameResult::Ready);for(unsigned j=0;j<3;++j)Near(software.positions[j].xyz[0],moved.positions[j].xyz[0]);
 }
 {Fixture high;M2CompBone bones[301]{};C44Matrix palette[301];Bind(high.data.m_bones,bones);high.model.m_boneMatrices=palette;high.combos[0]=300;palette[300].d0=9;Mesh wide;
  assert(BuildM2ShadowMesh(high.model,high.scene,0,wide)==FrameResult::Ready);Near(wide.positions[0].xyz[0],11);}
 Fixture f;Mesh mesh;textureLookups=0;f.model.m_textures=nullptr;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.indices.size()==3&&!textureLookups);
 f.model.m_textures=f.bindings;f.materials[0].m_blendMode=1;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&leases==1);Near(mesh.material.threshold,224.f/255);Near(mesh.coverage[0].uv[0][0],.25f);mesh={};assert(!leases);
 f.model.m_alpha=.54f;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.indices.empty());f.model.m_alpha=.55f;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&!mesh.indices.empty());mesh={};
 f.materials[0].m_blendMode=4;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.indices.empty());f.materials[0].m_flags=0x80;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.material.mode==CoverageMode::ForcedCutout);mesh={};
 --f.model.m_frameStamp;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::StalePose);
 f.materials[0].m_flags=0x40;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.indices.empty());
 f.materials[0].m_flags=0x80;f.batch[0].m_shader=0x8000;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::Ready&&mesh.indices.empty());f.batch[0].m_shader=0;
 f.Pose(0);f.indices[2]=99;assert(BuildM2ShadowMesh(f.model,f.scene,0,mesh)==FrameResult::InvalidGeometry);mesh={};f.indices[2]=1;
 f.model.m_fFlags=0;f.model.m_flags=0;PrepareM2ShadowPoses(f.scene);assert(initialisations==1&&schedules==1&&(f.model.m_fFlags&1));PrepareM2ShadowPoses(f.scene);assert(initialisations==1&&schedules==1);assert(!leases);
 std::cout<<"PASS restored M2 profile/palette/current-pose/source-UV/opacity/forced-shadow and deferred non-light scheduling tests\n";
}
