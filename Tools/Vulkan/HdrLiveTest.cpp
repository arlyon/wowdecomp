#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Buffer.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/HdrToneMapping.h"
#include "Engine/Source/Gx/Gx.h"
#include <SDL.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

namespace {
void Check(bool ok,const char* s) { if(!ok) throw std::runtime_error(s); }
struct Vertex {float x,y,z;uint32_t colour;};
CImVector Read(CGxDevice& gx,int x,int y) {
    CiRect r;r.minX=x;r.maxX=x+1;r.minY=y;r.maxY=y+1;
    TSGrowableArray<CImVector> values;gx.DeviceReadPixels(r,values);Check(values.Count()==1,"SDR readback missing");return values[0];
}
void NearByte(uint8_t value,float expected,const char* label) {if(std::abs(int(value)-int(std::lround(expected*255)))>2) std::cerr<<label<<" got="<<int(value)<<" expected="<<expected*255<<"\n";Check(std::abs(int(value)-int(std::lround(expected*255)))<=2,label);}
void Program(CGxShader& shader,EGxShTarget target,const uint32_t* words,size_t size) {
    shader.m_target=target;shader.m_code.SetCount(size);std::memcpy(shader.m_code.Ptr(),words,size);
}
}
int main() {
    try {
        Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        CGxDeviceVulkan gx;g_theGxDevicePtr=&gx;
        CGxFormat format;format.m_window=1;format.m_maximize=0;format.m_size={320,240};
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"device creation");gx.m_cursorVisible=0;
        Check(gx.SceneHdrSupported(),"required HDR transport unsupported");
        Check(!gx.SceneSetHdr(true,std::numeric_limits<float>::quiet_NaN()),"NaN EV accepted");
        Check(!gx.SceneSetHdr(true,9),"out-of-range EV accepted");
        gx.RsSet(GxRs_Culling,0);gx.RsSet(GxRs_DepthTest,0);gx.RsSet(GxRs_DepthWrite,0);gx.RsSet(GxRs_Fog,0);gx.RsSet(GxRs_ColorWrite,15);gx.RsSet(GxRs_BlendingMode,0);
        C44Matrix identity;gx.XformPushAndSet(GxXform_World,identity);gx.XformSetView(identity);gx.XformSetProjection(identity);
        auto* pool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,6*sizeof(Vertex),GxPoolHintBit_Unk3,"HDR real Gx fixture");
        auto* buffer=gx.BufCreate(pool,sizeof(Vertex),6,0);
        const std::array<Vertex,6> vertices{{{-1,-1,0,0xffffffff},{1,-1,0,0xffffffff},{-1,1,0,0xffffffff},{-1,1,0,0xffffffff},{1,-1,0,0xffffffff},{1,1,0,0xffffffff}}};
        gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
        const CGxVertexAttrib attributes[]={{GxVA_Position,4,0,sizeof(Vertex)},{GxVA_Color0,0,12,sizeof(Vertex)}};
        gx.PrimVertexFormat(buffer,attributes,2);gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0));
        CGxBatch batch{};batch.m_primType=GxPrim_Triangles;batch.m_count=6;
        CGxShader vs,ps;
        constexpr uint32_t vertex[]={0xfffe0200,0x0200001f,0x80000000,0x900f0000,0x02000001,0xc00f0000,0x90e40000,0x0000ffff};
        // Consume the maintained FOG varying with a runtime-zero coefficient;
        // avoid an unrelated unused-varying performance warning in this oracle.
        constexpr uint32_t pixel[]={0xffff0300,0x0200001f,0x8000000b,0x90010000,
            0x04000004,0x800f0800,0x90000000,0xa0e40001,0xa0e40000,0x0000ffff};
        Program(vs,GxSh_Vertex,vertex,sizeof(vertex));Program(ps,GxSh_Pixel,pixel,sizeof(pixel));
        gx.RsSet(GxRs_VertexShader,static_cast<void*>(&vs));gx.RsSet(GxRs_PixelShader,static_cast<void*>(&ps));
        const float zero[]={0,0,0,0};gx.ShaderConstantsSet(GxSh_Pixel,1,zero,1);
        const std::array<float,4> radiance{.01f,.5f,4.f,20.f};
        std::array<CImVector,4> classic{};bool classicSaved=false;
        unsigned frames=0;
        std::array<std::array<CImVector,4>,36> nonePictures{};
        for(unsigned look=0;look<3;++look) {
            std::array<float,24> lut{};
            for(unsigned b=0;b<2;++b) for(unsigned g=0;g<2;++g) for(unsigned r=0;r<2;++r) {
                const unsigned i=3*(r+2*(g+2*b));lut[i]=look==2?r*.5f:float(r);lut[i+1]=float(g);lut[i+2]=float(b);
            }
            Check(gx.SceneSetHdrLut(look?lut.data():nullptr,look?2:0),"LUT setter failed");
        for(float scale:{1.f,.5f,.75f}) for(bool taa:{false,true}) for(bool hdr:{false,true}) for(float ev:{0.f,-2.f,2.f}) {
            Check(gx.SceneSetHdr(hdr,ev),"HDR request failed");Check(gx.SceneSetRenderScale(scale),"scale request failed");
            gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
            const bool begun=gx.SceneBeginWorld(taa,1);Check(begun || (!hdr && !taa && scale==1),"world begin failed");
            Check(gx.SceneHdrActive()==hdr,"active admission mismatch");
            for(unsigned band=0;band<4;++band) {
                gx.XformSetViewport(band*.25f,(band+1)*.25f,0,1,0,1);
                const float encoded=GxVulkan::HdrEncodeSdr(radiance[band]);
                const float c[]={encoded,encoded,encoded,.375f};gx.ShaderConstantsSet(GxSh_Pixel,0,c,1);gx.Draw(&batch,0);
            }
            if(hdr) {
                for(unsigned band=0;band<4;++band) {
                    std::array<float,4> linear{};
                    Check(gx.SceneReadHdrPixel(uint32_t((band+.5f)*80*scale),uint32_t(120*scale),linear),"float readback missing");
                    Check(std::abs(linear[0]-radiance[band])<.0001f && std::abs(linear[3]-.375f)<1e-6f,"world HDR output boundary range/alpha");
                }
            }
            if(hdr) {
                // Explicit linear-emission producer: not another legacy decode.
                gx.SceneSetLinearOutput(true);gx.RsSet(GxRs_BlendingMode,GxBlend_NoAlphaAdd);gx.RsSet(GxRs_ColorWrite,7);
                const float emission[]={.25f,.25f,.25f,1};gx.ShaderConstantsSet(GxSh_Pixel,0,emission,1);
                for(unsigned band=2;band<4;++band) {
                    gx.XformSetViewport(band*.25f,(band+1)*.25f,0,1,0,1);gx.Draw(&batch,0);
                    std::array<float,4> linear{};Check(gx.SceneReadHdrPixel(uint32_t((band+.5f)*80*scale),uint32_t(120*scale),linear),"emission readback missing");
                    Check(std::abs(linear[0]-(radiance[band]+.25f))<.0001f && std::abs(linear[3]-.375f)<1e-6f,"linear emissive contribution clipped/alpha changed");
                }
                gx.SceneSetLinearOutput(false);gx.RsSet(GxRs_BlendingMode,GxBlend_Opaque);gx.RsSet(GxRs_ColorWrite,15);
            }
            if(hdr && look==0 && ev==0) {
                CiRect depthRect;depthRect.minX=160;depthRect.maxX=161;depthRect.minY=120;depthRect.maxY=121;
                TSGrowableArray<float> depths;gx.DeviceReadDepths(depthRect,depths);
                Check(depths.Count()==1 && std::isfinite(depths[0]),"native/scaled HDR depth readback");
            }
            gx.SceneEndWorld();Check(!gx.SceneHdrActive(),"HDR leaked outside world");
            // Real recovered PS UI draw after scene resolve, same exact bytes.
            gx.XformSetViewport(0,.125f,0,.125f,0,1);
            const float ui[]={37/255.f,123/255.f,219/255.f,173/255.f};gx.ShaderConstantsSet(GxSh_Pixel,0,ui,1);gx.Draw(&batch,0);
            auto u=Read(gx,10,230);if(u.r!=37 || u.g!=123 || u.b!=219 || u.a!=173) std::cerr<<"UI got "<<int(u.r)<<","<<int(u.g)<<","<<int(u.b)<<","<<int(u.a)<<" mode="<<hdr<<" scale="<<scale<<" taa="<<taa<<" look="<<look<<" EV="<<ev<<"\n";Check(u.r==37 && u.g==123 && u.b==219 && u.a==173,"UI graded or resized");
            for(unsigned band=0;band<4;++band) {
                const auto c=Read(gx,40+80*band,120);
                if(look==0) nonePictures[frames%36][band]=c;
                else if(look==1) {
                    const auto n=nonePictures[frames%36][band];
                    Check(std::abs(int(c.r)-n.r)<=1 && std::abs(int(c.g)-n.g)<=1 && std::abs(int(c.b)-n.b)<=1 && c.a==n.a,"identity LUT differs from None beyond one quantization code");
                }
                const float expected=hdr?GxVulkan::HdrEncodeSdr(GxVulkan::HdrToneMapLinear(radiance[band]+(band>=2?.25f:0.f),ev)):std::min(1.f,GxVulkan::HdrEncodeSdr(radiance[band]));
                NearByte(c.r,hdr && look==2?expected*.5f:expected,"SDR tone/LUT mismatch");NearByte(c.a,.375f,"alpha changed");
                if(!hdr && !taa && scale==1) {
                    if(classicSaved) Check(c.r==classic[band].r && c.g==classic[band].g && c.b==classic[band].b && c.a==classic[band].a,"off EV not exact");
                    else classic[band]=c;
                }
            }
            if(!hdr && !taa && scale==1) classicSaved=true;
            gx.ScenePresent();++frames;
        }
        }
        // A late HDR-only fault must retain earlier Classic fragments AND draw
        // the failing/remaining packets through original C0 into attachment0.
        gx.SceneSetRenderScale(1.f);gx.SceneSetHdr(true,2.f);
        gx.SceneHdrTestLimits(3,512ull*1024*1024,3);
        gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
        Check(gx.SceneBeginWorld(false,700),"rollback world begin");
        for(unsigned band=0;band<4;++band) {
            gx.XformSetViewport(band*.25f,(band+1)*.25f,0,1,0,1);
            const float encoded=GxVulkan::HdrEncodeSdr(radiance[band]);const float c[]={encoded,encoded,encoded,.375f};
            gx.ShaderConstantsSet(GxSh_Pixel,0,c,1);gx.Draw(&batch,0);
            if(band>=2) Check(!gx.SceneHdrActive(),"late fault retained HDR status");
        }
        gx.SceneEndWorld();
        for(unsigned band=0;band<4;++band) {auto c=Read(gx,40+80*band,120);NearByte(c.r,std::min(1.f,GxVulkan::HdrEncodeSdr(radiance[band])),"rollback omitted/regraded Classic fragment");NearByte(c.a,.375f,"rollback alpha");}
        gx.ScenePresent();
        gx.SceneHdrTestLimits(3,512ull*1024*1024,0);gx.SceneSetHdr(true,2);gx.SceneSetRenderScale(1);
        gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));Check(gx.SceneBeginWorld(false,702),"screen-copy scope");
        const float displayColour[]={.25f,.5f,.75f,.375f};gx.ShaderConstantsSet(GxSh_Pixel,0,displayColour,1);gx.Draw(&batch,0);
        CGxTexFlags copyFlags;copyFlags.m_renderTarget=1;CGxTex* copyTarget=nullptr;
        Check(gx.TexCreate(GxTex_2d,320,240,0,GxTex_Argb8888,GxTex_Argb8888,copyFlags,nullptr,nullptr,"HDR unsupported screen-copy oracle",copyTarget)!=0,"screen-copy target");
        Check(gx.TexCopy(copyTarget,{0,0},{0,0},{1,1},0,0)!=0 && !gx.SceneHdrActive(),"legacy screen-copy did not withdraw HDR");
        gx.SceneEndWorld();auto screenPixel=Read(gx,160,120);NearByte(screenPixel.r,.25f,"legacy-copy fallback regraded scene");NearByte(screenPixel.a,.375f,"legacy-copy alpha");gx.TexDestroy(copyTarget);gx.ScenePresent();
        gx.SceneHdrTestLimits(2,512ull*1024*1024,0);
        Check(!gx.SceneHdrSupported() && !gx.SceneSetHdr(true,0),"two-colour-slot device advertised three-slot HDR transport");
        gx.SceneHdrTestLimits(3,0,0);Check(gx.SceneSetHdr(true,0),"budget test request");
        gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(9,17,25,255));
        Check(gx.SceneBeginWorld(false,701) && !gx.SceneHdrActive(),"image-budget fallback partially committed HDR");
        gx.SceneClear(1,CImVector(15,31,63,127));gx.SceneEndWorld();auto budgetPixel=Read(gx,160,120);
        Check(budgetPixel.r==63 && budgetPixel.g==31 && budgetPixel.b==15 && budgetPixel.a==127,"budget fallback pixel/alpha");gx.ScenePresent();
        gx.SceneHdrTestLimits(3,512ull*1024*1024,0);gx.SceneSetHdrLut(nullptr,0);
        // Resize and native viewport ownership; only the scene rectangle is toned.
        for(unsigned transition=0;transition<3;++transition) {
            format.m_size=transition%2?C2iVector{400,300}:C2iVector{320,240};
            Check(gx.DeviceSetFormat(format)!=0,"HDR resize");gx.SceneSetRenderScale(transition%2?.75f:.5f);gx.SceneSetHdr(true,0);
            gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(9,17,25,255));
            gx.XformSetViewport(.2f,.8f,.2f,.8f,0,1);Check(gx.SceneBeginWorld(transition%2,710+transition),"resized HDR scope");
            const float encoded=GxVulkan::HdrEncodeSdr(.01f);const float c[]={encoded,encoded,encoded,.375f};gx.ShaderConstantsSet(GxSh_Pixel,0,c,1);gx.Draw(&batch,0);
            // Explicit offscreen colour target remains original/ungraded.
            CGxTexFlags flags;flags.m_renderTarget=1;CGxTex* target=nullptr;
            Check(gx.TexCreate(GxTex_2d,format.m_size.x,format.m_size.y,0,GxTex_Argb8888,GxTex_Argb8888,flags,nullptr,nullptr,"HDR ungraded offscreen",target)!=0,"offscreen create");
            gx.DeviceSetRenderTarget(GxBuffers_Color,target,0);gx.XformSetViewport(0,1,0,1,0,1);
            const float offscreen[]={.25f,.5f,.75f,.375f};gx.ShaderConstantsSet(GxSh_Pixel,0,offscreen,1);gx.Draw(&batch,0);
            auto p=Read(gx,format.m_size.x/2,format.m_size.y/2);NearByte(p.r,.25f,"offscreen decoded/graded");NearByte(p.a,.375f,"offscreen alpha");
            gx.DeviceSetRenderTarget(GxBuffers_Color,nullptr,0);gx.SceneEndWorld();
            auto outside=Read(gx,2,2);Check(outside.r==25 && outside.g==17 && outside.b==9 && outside.a==255,"HDR tone escaped scene viewport");
            auto inside=Read(gx,format.m_size.x/2,format.m_size.y/2);NearByte(inside.r,GxVulkan::HdrEncodeSdr(.01f),"resized dark scene changed");
            gx.TexDestroy(target);gx.ScenePresent();
        }
        // Exercise the production owner pump and worker, not a synchronous file
        // helper. Controlled fixtures only; generated max-domain LUT lives in
        // this test's build directory and is retained with the evidence.
        const std::string identityPath=std::string(WOWDECOMP_HDR_FIXTURE_DIR)+"/HdrIdentity.cube";
        const std::string invalid=std::string(WOWDECOMP_HDR_FIXTURE_DIR)+"/HdrInvalid.cube";
        const std::string missing=std::string(WOWDECOMP_HDR_FIXTURE_DIR)+"/missing.cube";
        const auto pump=[&](const std::string& path) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            do {
                gx.SceneRequestHdrLut(path.c_str());
                if(!gx.SceneHdrTestLutState()[2]) return;
                std::this_thread::yield();
            } while(std::chrono::steady_clock::now()<deadline);
            Check(false,"production LUT owner pump timed out");
        };
        Check(gx.SceneSetHdrLut(nullptr,0),"clear fixture LUT");
        gx.SceneRequestHdrLut(identityPath.c_str(),true);
        Check(gx.SceneHdrTestLutState()[2]==1,"owned LUT job not admitted");
        const auto reserved=gx.SceneHdrTestLutState()[0];
        // Cancel/supersede without waiting: old job remains charged until drain.
        gx.SceneRequestHdrLut(missing.c_str(),true);
        Check(gx.SceneHdrTestLutState()[0]==reserved,"cancelled job reservation released before drain");
        pump(missing);
        Check(std::string(gx.SceneHdrLutStatus()).find("retained")!=std::string::npos,"missing file reported success/None");
        gx.SceneRequestHdrLut(identityPath.c_str(),true);pump(identityPath);
        Check(gx.SceneHdrTestLutState()[1]==2,"valid production LUT not installed");
        gx.SceneRequestHdrLut(invalid.c_str(),true);pump(invalid);
        Check(gx.SceneHdrTestLutState()[1]==2 && std::string(gx.SceneHdrLutStatus()).find("retained")!=std::string::npos,"invalid LUT discarded current LUT");
        // Same-path Apply retries, and A/B/A generations never commit stale A.
        gx.SceneRequestHdrLut(identityPath.c_str(),true);
        gx.SceneRequestHdrLut(invalid.c_str(),true);
        gx.SceneRequestHdrLut(identityPath.c_str(),true);pump(identityPath);
        Check(gx.SceneHdrTestLutState()[1]==2,"superseded Apply lost newest request");
        Check(gx.SceneSetHdrLut(nullptr,0),"clear small LUT before maximum-domain fixture");
        gx.SceneClear(3,CImVector(0,0,0,255));gx.ScenePresent();
        const std::string maximum=(std::filesystem::current_path()/"hdr-controlled-max.cube").string();
        const std::string maximumLook=(std::filesystem::current_path()/"hdr-controlled-max-look.cube").string();
        for(bool look:{false,true}) {
            std::ofstream fixture(look?maximumLook:maximum);fixture<<"LUT_3D_SIZE 33\n";
            for(unsigned b=0;b<33;++b) for(unsigned g=0;g<33;++g) for(unsigned r=0;r<33;++r)
                fixture<<(look?r/64.f:r/32.f)<<' '<<g/32.f<<' '<<b/32.f<<'\n';
        }
        const auto drawLut=[&](bool read) {
            Check(gx.SceneSetRenderScale(1) && gx.SceneSetHdr(true,0),"max LUT scene setup");
            gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
            Check(gx.SceneBeginWorld(false,900),"max LUT world begin");
            const float encoded=GxVulkan::HdrEncodeSdr(.25f);
            const float colour[]={encoded,encoded,encoded,.375f};
            gx.ShaderConstantsSet(GxSh_Pixel,0,colour,1);gx.Draw(&batch,0);gx.SceneEndWorld();
            return read?Read(gx,format.m_size.x/2,format.m_size.y/2):CImVector{};
        };
        gx.SceneRequestHdrLut(maximum.c_str(),true);pump(maximum);
        Check(gx.SceneHdrTestLutState()[1]==33,"maximum LUT clean admission failed");
        const auto memoryA=gx.SceneHdrTestLutMemory();
        Check(memoryA[0]==33u*33u*33u*12u && memoryA[1]==memoryA[0],"retained CPU LUT is not exact compact RGB");
        Check(memoryA[4]==0 && memoryA[6]==1,"ready job reservation survived ownership move or output was copied");
        const auto pictureA=drawLut(true);
        NearByte(pictureA.r,GxVulkan::HdrEncodeSdr(.25f),"max identity LUT pixel");
        NearByte(pictureA.a,.375f,"max identity output alpha changed");
        // Record and submit another A scene, then replace before the owner has
        // observed/drained that fence. Old CPU and GPU storage must coexist.
        drawLut(false);gx.ScenePresent();
        gx.SceneRequestHdrLut(maximumLook.c_str(),true);pump(maximumLook);
        const auto memoryB=gx.SceneHdrTestLutMemory();
        Check(gx.SceneHdrTestLutState()[1]==33 && std::string(gx.SceneHdrLutStatus()).find("Loaded")!=std::string::npos,"max33 A-to-B replacement refused");
        Check(memoryB[1]==memoryA[1] && memoryB[3]>=memoryA[2] && memoryB[4]==0 && memoryB[6]==1,"CPU move/old GPU overlap accounting lost");
        Check(memoryB[5]<=2u*1024u*1024u,"maximum replacement peak exceeded cap");
        const auto retainedB=gx.SceneHdrTestLutState();
        gx.SceneRequestHdrLut(maximum.c_str(),true);pump(maximum);
        Check(std::string(gx.SceneHdrLutStatus()).find("refused")!=std::string::npos,"third live generation exceeded real budget");
        Check(gx.SceneHdrTestLutState()[1]==33 && gx.SceneHdrTestLutState()[3]==retainedB[3],"third generation refusal discarded current/retired LUT");
        const auto pictureB=drawLut(true); // ordinary BeginFrame fence drain
        NearByte(pictureB.r,GxVulkan::HdrEncodeSdr(.25f)*.5f,"max look/refusal changed current pixels");
        NearByte(pictureB.g,GxVulkan::HdrEncodeSdr(.25f),"max look changed green");
        NearByte(pictureB.a,.375f,"max look output alpha changed");
        Check(pictureB.a==pictureA.a,"max LUT replacement changed exact output alpha");
        Check(int(pictureA.r)-int(pictureB.r)>50,"max A and B produced indistinguishable grading");
        Check(gx.SceneHdrTestLutState()[3]==0,"ordinary frame did not retire old LUT");
        gx.SceneRequestHdrLut(maximum.c_str(),true);pump(maximum);
        Check(std::string(gx.SceneHdrLutStatus()).find("Loaded")!=std::string::npos,"ordinary fence drain/retry did not admit max LUT");
        const auto pictureRetry=drawLut(true);
        Check(pictureRetry.r==pictureA.r && pictureRetry.g==pictureA.g && pictureRetry.b==pictureA.b && pictureRetry.a==pictureA.a,"max retry failed exact RGB/alpha identity");
        gx.ScenePresent();
        // Existing swapchain recreation reuploads retained CPU data by alias;
        // same geometry, no extra CPU owner or replacement copy is permitted.
        Check(gx.DeviceSetFormat(format)!=0,"retained LUT reupload setup");
        const auto pictureAlias=drawLut(true);
        const auto alias=gx.SceneHdrTestLutMemory();
        Check(alias[0]==memoryA[0] && alias[1]==memoryA[1] && alias[3]==0 && alias[4]==0,"retained CPU alias/recreation accounting changed");
        Check(alias[7]==gx.SceneHdrTestLutState()[0],"retained CPU LUT alias counted twice during GPU reupload");
        Check(pictureAlias.r==pictureA.r && pictureAlias.g==pictureA.g && pictureAlias.b==pictureA.b && pictureAlias.a==pictureA.a,"retained CPU alias reupload changed exact pixels");
        gx.ScenePresent();
        std::cout<<"PASS: compact max33 A-to-B and third-generation refusal/drain/retry; CPU size/capacity="<<memoryB[0]<<"/"<<memoryB[1]
            <<" GPU="<<memoryB[2]<<" retired="<<memoryB[3]<<" replacement peak="<<memoryB[5]<<" committed overlap="<<retainedB[0]<<" moved="<<memoryB[6]<<" alias upload="<<alias[7]<<"\n";
        gx.SceneRequestHdrLut("",true);pump("");
        gx.BufDestroy(buffer);gx.PoolDestroy(pool);gx.DeviceDestroy();
        Check(!gx.SceneHdrSupported(),"destroyed device advertised HDR");
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0 && gx.SceneHdrSupported(),"HDR device recreation");
        gx.SceneSetHdr(true,2);gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(15,31,63,127));
        auto noWorld=Read(gx,20,20);Check(noWorld.r==63 && noWorld.g==31 && noWorld.b==15 && noWorld.a==127,"no-world frame was graded");
        gx.ScenePresent();gx.DeviceDestroy();Check(gx.SceneHdrTestValidationClean(),"Vulkan validation warnings/errors emitted");SDL_Quit();
        std::cout<<"PASS: real Gx recovered Draw -> FP32 .01/.5/4/20 -> linear histories/scaling -> manual EV SDR -> exact native UI, "<<frames<<" frames\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
