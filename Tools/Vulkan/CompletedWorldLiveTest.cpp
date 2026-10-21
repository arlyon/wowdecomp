#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Gx.h"
#include <SDL.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Vertex { float x,y,z; uint32_t colour; };
CImVector ReadPixel(CGxDeviceVulkan& gx, int x, int y) {
    CiRect rect; rect.minX=x; rect.maxX=x+1; rect.minY=y; rect.maxY=y+1;
    TSGrowableArray<CImVector> pixels; gx.DeviceReadPixels(rect,pixels);
    Check(pixels.Count()==1,"Missing completed-world readback");
    return pixels[0];
}
void Pixel(CGxDeviceVulkan& gx, int x, int y, CImVector expected) {
    const auto p=ReadPixel(gx,x,y);
    Check(std::abs(int(p.r)-expected.r)<=1 && std::abs(int(p.g)-expected.g)<=1 &&
        std::abs(int(p.b)-expected.b)<=1 && std::abs(int(p.a)-expected.a)<=1,"Completed world/UI colour mismatch");
}
float Depth(CGxDeviceVulkan& gx, int x, int y) {
    CiRect rect; rect.minX=x; rect.maxX=x+1; rect.minY=y; rect.maxY=y+1;
    TSGrowableArray<float> depths; gx.DeviceReadDepths(rect,depths);
    Check(depths.Count()==1,"Missing completed-world depth"); return depths[0];
}
GxWorld::WorldFrameDescription Description(CGxDeviceVulkan& gx, uint64_t scene) {
    CRect size; gx.CapsWindowSize(size);
    GxWorld::WorldFrameDescription frame; frame.scene=scene;
    frame.width=uint32_t(size.maxX-size.minX); frame.height=uint32_t(size.maxY-size.minY);
    frame.viewport={frame.width/4,frame.height/4,frame.width/2,frame.height/2};
    const auto& stack=gx.m_xforms[GxXform_View];
    const auto& view=stack.m_mtx[stack.m_level];
    static_assert(sizeof(view)==sizeof(frame.view));
    std::memcpy(frame.view.data(),&view,sizeof(view));
    std::memcpy(frame.projection.data(),&gx.m_projection,sizeof(gx.m_projection));
    return frame;
}
struct Sdl { Sdl(){Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());} ~Sdl(){SDL_Quit();} };
}
int main(int argc, char** argv) {
    try {
        Check(argc==1 || (argc==2 && (!std::strcmp(argv[1],"--hdr") || !std::strcmp(argv[1],"--taa") ||
            !std::strcmp(argv[1],"--scaled"))),"Unknown completed-world fixture mode");
        const bool hdr=argc==2 && !std::strcmp(argv[1],"--hdr");
        const bool taa=argc==2 && !std::strcmp(argv[1],"--taa");
        const bool scaled=argc==2 && !std::strcmp(argv[1],"--scaled");
        Sdl sdl;
        CGxDeviceVulkan gx; g_theGxDevicePtr=&gx;
        Check(!gx.SceneComposeCompletedWorld(77),"Uncreated renderer exposed a completed image");
        CGxFormat format; format.m_window=1; format.m_maximize=0; format.m_size={320,240}; format.m_vsync=0;
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"Completed-world native device creation failed");
        gx.m_cursorVisible=0;
        gx.RsSet(GxRs_VertexShader,static_cast<void*>(nullptr)); gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
        gx.RsSet(GxRs_Culling,0); gx.RsSet(GxRs_DepthTest,1); gx.RsSet(GxRs_DepthWrite,1);
        gx.RsSet(GxRs_DepthFunc,0); gx.RsSet(GxRs_BlendingMode,0); gx.RsSet(GxRs_ColorWrite,15); gx.RsSet(GxRs_Fog,0);
        C44Matrix identity; gx.XformPushAndSet(GxXform_World,identity); gx.XformSetView(identity); gx.XformSetProjection(identity);
        auto* pool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,6*sizeof(Vertex),GxPoolHintBit_Unk3,"Completed world fixture");
        auto* buffer=gx.BufCreate(pool,sizeof(Vertex),6,0);
        std::array<Vertex,6> vertices{{{-1,-1,0,0xFFE02810},{1,-1,0,0xFFE02810},{-1,1,0,0xFFE02810},
            {-1,1,0,0xFFE02810},{1,-1,0,0xFFE02810},{1,1,0,0xFFE02810}}};
        const CGxVertexAttrib attributes[]={{GxVA_Position,4,0,sizeof(Vertex)},{GxVA_Color0,0,12,sizeof(Vertex)}};
        const auto bind=[&]{gx.PrimVertexFormat(buffer,attributes,2);gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0));};
        CGxBatch batch{}; batch.m_primType=GxPrim_Triangles; batch.m_count=6;
        const CImVector background(9,17,25,255);
        Check(gx.SceneSetHdr(hdr,0),"Completed-world HDR mode unavailable");
        Check(gx.SceneSetRenderScale(scaled?.5f:1.f),"Completed-world scale unavailable");
        gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,background);
        Check(!gx.SceneComposeCompletedWorld(77),"Missing first world frame was exposed");
        gx.XformSetViewport(.25f,.75f,.25f,.75f,0,1);
        Check(gx.SceneBeginWorld(taa,77),"Original offscreen world path unavailable");
        gx.BufData(buffer,vertices.data(),sizeof(vertices),0);bind();gx.Draw(&batch,0);gx.SceneEndWorld();
        const auto worldColour=ReadPixel(gx,160,120);
        Check(int(worldColour.r)>int(worldColour.b)+20,"World fixture produced no meaningful coloured geometry");
        const auto captured=Description(gx,77);
        Check(gx.SceneCaptureCompletedWorld(captured),"Final world capture failed");
        Check(!gx.SceneComposeCompletedWorld(77),"Unsubmitted generation became displayable");
        gx.ScenePresent();

        // No new world scope/draw/capture runs in this loop. Only current UI is
        // drawn; the final world pixels/depth/camera are an earlier generation.
        GxWorld::WorldFrame displayed; uint64_t generation=0;
        C44Matrix changed; changed.d0=11; gx.XformSetView(changed);
        for(unsigned frame=0;frame<20;++frame) {
            gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,background);
            Check(gx.SceneComposeCompletedWorld(77,&displayed),"UI-only frame lost completed output");
            if(!generation)generation=displayed.lease.generation;
            Check(displayed.lease.generation==generation && displayed.description.view==captured.view &&
                displayed.description.projection==captured.projection,"Re-presentation changed generation/camera identity");
            Pixel(gx,160,120,worldColour);Pixel(gx,2,2,background);
            Check(std::abs(Depth(gx,160,120)-.5f)<1e-5f,"Completed world depth did not follow colour");
            Check(std::abs(Depth(gx,2,2)-1.f)<1e-5f,"Composition leaked uninitialized depth outside world viewport");
            const uint32_t colour=frame%2?0xFF20D040:0xFF4060D0;
            for(auto& vertex:vertices)vertex.colour=colour;
            gx.BufData(buffer,vertices.data(),sizeof(vertices),0);bind();
            gx.XformSetView(identity);gx.XformSetViewport(.4f,.6f,.4f,.6f,0,1);
            gx.RsSet(GxRs_DepthTest,0);gx.RsSet(GxRs_DepthWrite,0);gx.Draw(&batch,0);
            Pixel(gx,160,120,CImVector(uint8_t(colour),uint8_t(colour>>8),uint8_t(colour>>16),255));
            gx.XformSetView(changed);gx.ScenePresent();
        }
        // Output owns its images, not a live geometry/callback dependency.
        gx.BufDestroy(buffer);
        gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);
        Check(gx.SceneComposeCompletedWorld(77,&displayed),"Source deletion invalidated copied output");
        Pixel(gx,160,120,worldColour);
        gx.SceneInvalidateCompletedWorld();
        Check(!gx.SceneComposeCompletedWorld(77),"Invalidated output was resurrected");
        Check(!gx.SceneComposeCompletedWorld(78),"Old scene appeared in new scene");
        Check(!gx.SceneComposeCompletedWorld(77),"Returning to an old scene resurrected its image");
        Check(gx.SceneCaptureCompletedWorld(Description(gx,77)),"Pre-resize capture failed");
        gx.ScenePresent();
        gx.SceneClear(3,background);
        Check(gx.SceneComposeCompletedWorld(77,&displayed),"Pre-resize image was not ready");
        const auto beforeResize=displayed.lease;gx.ScenePresent();
        format.m_size={384,256};Check(gx.DeviceSetFormat(format)!=0,"Completed-world resize failed");
        gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,background);
        Check(!gx.SceneComposeCompletedWorld(77),"Resize reused an incompatible generation");
        Check(gx.SceneCaptureCompletedWorld(Description(gx,77)),"Post-resize capture failed");gx.ScenePresent();
        gx.SceneClear(3,background);
        Check(gx.SceneComposeCompletedWorld(77,&displayed) && displayed.lease.epoch!=beforeResize.epoch &&
            displayed.lease.generation>beforeResize.generation,"Resize recycled a displayed image identity");
        gx.ScenePresent();
        Check(gx.SceneHdrTestValidationClean(),"Vulkan validation reported a completed-world lifetime/layout error");
        gx.DeviceDestroy();gx.DeviceDestroy();g_theGxDevicePtr=nullptr;
        std::cout<<"Completed world native backend: HDR="<<hdr<<" TAA="<<taa<<" scaled="<<scaled
            <<"; 1 world render, 20 independently drawn UI frames, exact contained final colour/depth, frozen camera, source deletion, resize and epoch invalidation passed (not original-UI cadence acceptance)\n";
        return 0;
    } catch(const std::exception& error){g_theGxDevicePtr=nullptr;std::cerr<<error.what()<<'\n';return 1;}
}
