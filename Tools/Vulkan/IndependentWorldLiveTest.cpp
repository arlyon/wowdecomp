#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include <SDL.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Vertex{float x,y,z;uint32_t colour;};
void TextureRows(EGxTexCommand command,uint32_t,uint32_t,uint32_t,uint32_t,void* data,uint32_t& stride,const void*& pixels){
    if(command==GxTex_Latch){stride=4;pixels=data;}
}
CImVector Pixel(CGxDeviceVulkan& gx){CiRect r(120,160,121,161);TSGrowableArray<CImVector> p;gx.DeviceReadPixels(r,p);Check(p.Count()==1,"Missing UI pixels");return p[0];}
void Equal(CImVector a,CImVector b){Check(a.r==b.r&&a.g==b.g&&a.b==b.b,"Independent world output changed");}
}
int main(int argc,char** argv){
 try{
  Check(argc==1 || (argc==2 && (!std::strcmp(argv[1],"--hdr") || !std::strcmp(argv[1],"--taa") || !std::strcmp(argv[1],"--scaled"))),"Unknown producer fixture mode");
  const bool hdr=argc==2&&!std::strcmp(argv[1],"--hdr"),taa=argc==2&&!std::strcmp(argv[1],"--taa"),scaled=argc==2&&!std::strcmp(argv[1],"--scaled");
  Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
  CGxDeviceVulkan gx;g_theGxDevicePtr=&gx;CGxFormat format;format.m_window=1;format.m_maximize=0;format.m_size={320,240};format.m_vsync=0;
  Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"Device creation failed");gx.m_cursorVisible=0;
  Check(gx.SceneConfigureIndependentWorld(true),"Independent world unavailable");
  Check(gx.SceneSetHdr(hdr,0) && gx.SceneSetRenderScale(scaled?.5f:1.f),"Producer quality mode unavailable");
  gx.RsSet(GxRs_VertexShader,static_cast<void*>(nullptr));gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
  gx.RsSet(GxRs_Culling,0);gx.RsSet(GxRs_DepthTest,0);gx.RsSet(GxRs_DepthWrite,1);gx.RsSet(GxRs_BlendingMode,0);gx.RsSet(GxRs_ColorWrite,15);gx.RsSet(GxRs_Fog,0);
  C44Matrix identity;gx.XformPushAndSet(GxXform_World,identity);gx.XformSetView(identity);gx.XformSetProjection(identity);
  auto* pool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,6*sizeof(Vertex),GxPoolHintBit_Unk3,"Independent world source");
  auto* buffer=gx.BufCreate(pool,sizeof(Vertex),6,0);
  Vertex vertices[]={{-1,-1,0,0},{1,-1,0,0},{-1,1,0,0},{-1,1,0,0},{1,-1,0,0},{1,1,0,0}};
  CGxVertexAttrib attributes[]={{GxVA_Position,4,0,sizeof(Vertex)},{GxVA_Color0,0,12,sizeof(Vertex)}};
  CGxBatch batch{};batch.m_primType=GxPrim_Triangles;batch.m_count=6;
  const auto draw=[&](uint32_t colour){for(auto& v:vertices)v.colour=colour;gx.BufData(buffer,vertices,sizeof(vertices),0);gx.PrimVertexFormat(buffer,attributes,2);gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0));gx.Draw(&batch,0);};
  uint32_t texel=0xFFFFFFFF;CGxTex* texture=nullptr;CGxTexFlags textureFlags;
  Check(gx.TexCreate(GxTex_2d,1,1,1,GxTex_Argb8888,GxTex_Argb8888,textureFlags,&texel,TextureRows,"Immutable world texture",texture),"World texture creation failed");
  gx.RsSet(GxRs_Texture0,static_cast<void*>(texture));
  unsigned generations=0;int motionOwner=0;
  const auto produce=[&](uint32_t colour){
   Check(gx.SceneBeginWorldProduction(77),"Producer was not ready");
   gx.XformSetViewport(.25f,.75f,.25f,.75f,0,1);Check(gx.SceneBeginWorld(taa,77),"World scope failed");
   if(taa && generations)Check(gx.SceneWorldTestHistoryValid(),"UI-only gaps invalidated temporal world history");
   ++generations;gx.SceneSetMotionOwner(&motionOwner,true);draw(colour);gx.SceneEndWorld();
   GxWorld::WorldFrameDescription frame;frame.scene=77;frame.width=320;frame.height=240;frame.viewport={80,60,160,120};
   std::memcpy(frame.view.data(),&identity,sizeof(identity));std::memcpy(frame.projection.data(),&identity,sizeof(identity));
   return gx.SceneEndWorldProduction(frame);
  };
  const CImVector background(3,7,11,255),red(16,40,224,255),green(32,208,64,255);
  gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);const auto first=produce(0xFFE02810);
  Check(!gx.SceneComposeCompletedWorld(77),"Unsubmitted output was exposed");gx.ScenePresent();
  GxWorld::WorldFrame shown;bool ready=false;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!ready&&std::chrono::steady_clock::now()<deadline){gx.SceneClear(3,background);ready=gx.SceneComposeCompletedWorld(77,&shown);gx.ScenePresent();std::this_thread::yield();}
  Check(ready&&shown.lease==first,"First world never completed");gx.SceneClear(3,background);Check(gx.SceneComposeCompletedWorld(77,&shown),"Completed output missing");{const auto p=Pixel(gx);if(hdr)Check(int(p.r)>int(p.g)+20,"HDR producer lost colour");else Equal(p,red);}gx.ScenePresent();
  gx.SceneWorldTestHoldProducer(true);gx.SceneClear(3,background);const auto second=produce(0xFF40D020);
  Check(gx.SceneComposeCompletedWorld(77,&shown)&&shown.lease==first,"Pending world replaced completed image");gx.ScenePresent();
  const auto start=std::chrono::steady_clock::now();unsigned presents=0;
  while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(500)){
   gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);
   Check(!gx.SceneWorldProductionReady(),"Held producer was declared ready");
   Check(gx.SceneComposeCompletedWorld(77,&shown)&&shown.lease==first,"CPU hold lost the old world image");
   // Mutate the original static geometry while its prior immutable GPU version
   // belongs to the held world command buffer. Current UI must remain current.
   if(!presents){texel=0xFF0080FF;gx.TexMarkForUpdate(texture,CiRect(0,0,1,1),1);}
   gx.XformSetViewport(.4f,.6f,.4f,.6f,0,1);draw(presents%2?0xFF10A0F0:0xFF8020A0);
   gx.ScenePresent();++presents;
  }
  Check(presents>=15,"Producer CPU hold blocked UI presentation");
  gx.BufDestroy(buffer);gx.RsSet(GxRs_Texture0,static_cast<void*>(nullptr));gx.TexDestroy(texture);texture=nullptr;
  gx.SceneWorldTestHoldProducer(false);ready=false;
  const auto finish=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!ready&&std::chrono::steady_clock::now()<finish){gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);Check(gx.SceneComposeCompletedWorld(77,&shown),"Released producer lost output");ready=shown.lease==second;gx.ScenePresent();std::this_thread::yield();}
  Check(ready,"Held generation never published");gx.SceneClear(3,background);Check(gx.SceneComposeCompletedWorld(77,&shown),"Released output missing");{const auto p=Pixel(gx);if(hdr)Check(int(p.g)>int(p.r)+20,"HDR producer lost new colour");else Equal(p,green);}gx.ScenePresent();
  gx.SceneInvalidateCompletedWorld();gx.SceneClear(3,background);Check(!gx.SceneComposeCompletedWorld(77),"Invalidation resurrected old output");gx.ScenePresent();
  Check(gx.SceneConfigureIndependentWorld(false),"Synchronous fallback failed");
  Check(gx.SceneConfigureIndependentWorld(true),"Shutdown fixture could not re-enable scheduling");
  gx.SceneWorldTestHoldProducer(true);gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);
  Check(gx.SceneBeginWorldProduction(88)&&gx.SceneBeginWorld(false,88),"Shutdown producer recording failed");
  gx.SceneClear(3,background);gx.SceneEndWorld();GxWorld::WorldFrameDescription final;final.scene=88;gx.SceneEndWorldProduction(final);gx.ScenePresent();
  const auto closing=std::chrono::steady_clock::now();gx.DeviceDestroy();
  Check(std::chrono::steady_clock::now()-closing<std::chrono::seconds(5),"Shutdown waited on a held application dependency");
  Check(gx.SceneHdrTestValidationClean(),"Vulkan validation diagnostics emitted");g_theGxDevicePtr=nullptr;SDL_Quit();
  std::cout<<"Same-device independent producer: HDR="<<hdr<<" TAA="<<taa<<" scaled="<<scaled<<" "<<presents<<" UI presents during 500ms CPU hold; immutable geometry/textures survive mutation/deletion; latest completed publication and synchronous rollback passed\n";return 0;
 }catch(const std::exception& e){g_theGxDevicePtr=nullptr;std::cerr<<e.what()<<'\n';return 1;}
}
