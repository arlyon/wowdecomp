#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/Buffer.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/WorldTarget.h"
#include <SDL.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Vertex { float x,y,z; uint32_t colour; };
void Pixel(CGxDeviceVulkan& gx, int x, int y, CImVector expected) {
    CiRect rect; rect.minX=x; rect.maxX=x+1; rect.minY=y; rect.maxY=y+1;
    TSGrowableArray<CImVector> pixels; gx.DeviceReadPixels(rect,pixels);
    Check(pixels.Count()==1,"Missing live readback");
    const auto p=pixels[0];
    Check(std::abs(int(p.r)-expected.r)<=1 && std::abs(int(p.g)-expected.g)<=1 &&
        std::abs(int(p.b)-expected.b)<=1 && std::abs(int(p.a)-expected.a)<=1,"Live viewport/UI pixel mismatch");
}
}
int main(int argc,char** argv) {
    try {
        Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        CGxDeviceVulkan gx;
        Check(!gx.SceneRenderScaleSupported(),"Uncreated device advertised scene scaling");
        Check(!gx.SceneSetRenderScale(.5f) && gx.SceneSetRenderScale(1.f),"Unsupported scale fallback changed");
        g_theGxDevicePtr = &gx;
        CGxFormat format; format.m_window=1; format.m_maximize=0; format.m_size={320,240};
        Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"Live device creation failed");
        if (!gx.SceneTemporalSupported()) { std::cout << "Live TAA unsupported on this device/surface\n"; return 77; }
        gx.m_cursorVisible=0;
        gx.RsSet(GxRs_VertexShader,static_cast<void*>(nullptr)); gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
        gx.RsSet(GxRs_Culling,0); gx.RsSet(GxRs_DepthTest,1); gx.RsSet(GxRs_DepthWrite,1);
        gx.RsSet(GxRs_DepthFunc,0); gx.RsSet(GxRs_BlendingMode,0); gx.RsSet(GxRs_ColorWrite,15);
        gx.RsSet(GxRs_Fog,0);
        C44Matrix identity; gx.XformPushAndSet(GxXform_World,identity); gx.XformSetView(identity); gx.XformSetProjection(identity);
        auto* pool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,12*sizeof(Vertex),GxPoolHintBit_Unk3,"Temporal synthetic vertices");
        auto* buffer=gx.BufCreate(pool,sizeof(Vertex),12,0);
        constexpr float edge=.004375f; // x=160.35 in the initial world viewport.
        std::array<Vertex,12> vertices{{{-1,-1,0,0xFFFFFFFF},{edge,-1,0,0xFFFFFFFF},{-1,1,0,0xFFFFFFFF},
            {-1,1,0,0xFFFFFFFF},{edge,-1,0,0xFFFFFFFF},{edge,1,0,0xFFFFFFFF},
            {edge,-1,0,0xFF000000},{1,-1,0,0xFF000000},{edge,1,0,0xFF000000},
            {edge,1,0,0xFF000000},{1,-1,0,0xFF000000},{1,1,0,0xFF000000}}};
        gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
        const CGxVertexAttrib attributes[]={{GxVA_Position,4,0,sizeof(Vertex)},
            {GxVA_Color0,0,12,sizeof(Vertex)}};
        gx.PrimVertexFormat(buffer,attributes,2); gx.PrimVertexMask((1u<<GxVA_Position)|(1u<<GxVA_Color0));
        CGxBatch batch{}; batch.m_primType=GxPrim_Triangles; batch.m_count=6;
        CGxShader vertexShader, pixelShader;
        constexpr uint32_t rigid[] = {0xFFFE0200,0x0200001F,0x80000000,0x900F0000,
            0x03000002,0xC00F0000,0x90E40000,0xA0E40000,0x0000FFFF};
        constexpr uint32_t pixel[] = {0xFFFF0200,0x02000001,0x800F0800,0xA0E40000,0x0000FFFF};
        vertexShader.m_target=GxSh_Vertex; vertexShader.m_code.SetCount(sizeof(rigid));
        std::memcpy(vertexShader.m_code.Ptr(),rigid,sizeof(rigid));
        pixelShader.m_target=GxSh_Pixel; pixelShader.m_code.SetCount(sizeof(pixel));
        std::memcpy(pixelShader.m_code.Ptr(),pixel,sizeof(pixel));
        int owner;
        if (argc==2 && std::strcmp(argv[1],"--upscale")==0) {
            if (!gx.SceneRenderScaleSupported()) { std::cout << "Scene scale unsupported\n"; return 77; }
            Check(!gx.SceneSetRenderScale(std::numeric_limits<float>::quiet_NaN()),"NaN scene scale accepted");
            Check(!gx.SceneSetRenderScale(std::numeric_limits<float>::infinity()),"Infinite scene scale accepted");
            Check(!gx.SceneSetRenderScale(.49f) && !gx.SceneSetRenderScale(1.01f),"Out-of-range scene scale accepted");
            // Real default-target rasterisation at 50/75/100%, native-coordinate
            // partial viewports, readback splits, UI and Pause-before-FFX.
            for(float scale:{.5f,.75f,1.f}) for(bool temporal:{false,true}) {
                Check(gx.SceneSetRenderScale(scale),"Scene scale rejected");
                for(int frame=0;frame<5;++frame) {
                    gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,CImVector(9,17,25,255));
                    gx.XformSetViewport(.17f,.83f,.22f,.78f,.2f,.8f);
                    bool begun=gx.SceneBeginWorld(temporal,frame==3 ? 12 : 11);
                    // Raster at scale=1 without the engineering target is the
                    // existing direct-output fallback, not a required scope.
                    Check(begun || (!temporal && scale==1.f),"Scaled scene begin failed");
                    gx.SceneSetMotionOwner(&owner,true);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    CiRect rect; rect.minX=100; rect.maxX=101; rect.minY=100; rect.maxY=101;
                    TSGrowableArray<float> depths; gx.DeviceReadDepths(rect,depths);
                    Check(depths.Count()==1 && std::abs(depths[0]-.5f)<1e-5f,"Scaled native depth readback mismatch");
                    Pixel(gx,100,100,CImVector(255,255,255,255));
                    Pixel(gx,2,2,CImVector(9,17,25,255));
                    // Explicit native targets inside a reduced scope retain
                    // their own extent/default depth and never acquire motion.
                    CGxTexFlags flags; flags.m_renderTarget=1;
                    CGxTex* screen=nullptr;
                    Check(gx.TexCreate(GxTex_2d,320,240,0,GxTex_Argb8888,GxTex_Argb8888,
                        flags,nullptr,nullptr,"Scaled FFX",screen)!=0,"Scaled FFX texture creation failed");
                    Check(gx.TexCopy(screen,{0,0},{0,0},{320,240},0,0)!=0,"In-scope native screen copy failed");
                    gx.DeviceSetRenderTarget(GxBuffers_Color,screen,0);
                    gx.RsSet(GxRs_DepthTest,0);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    gx.RsSet(GxRs_DepthTest,1);
                    gx.DeviceSetRenderTarget(GxBuffers_Color,nullptr,0);
                    gx.ScenePauseWorldMotion();
                    // FFX native screen copy, explicit-target pass, default output.
                    Check(gx.TexCopy(screen,{0,0},{0,0},{320,240},0,0)!=0,"Scaled FFX screen copy failed");
                    gx.RsSet(GxRs_PixelShader,static_cast<void*>(&pixelShader));
                    const float effectColour[]={.2f,.4f,.6f,1};
                    gx.ShaderConstantsSet(GxSh_Pixel,0,effectColour,1); gx.RsSet(GxRs_DepthTest,0);
                    gx.DeviceSetRenderTarget(GxBuffers_Color,screen,0);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    gx.DeviceSetRenderTarget(GxBuffers_Color,nullptr,0);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    gx.SceneEndWorld();
                    Pixel(gx,100,100,CImVector(153,102,51,255));
                    TSGrowableArray<float> outputDepths; gx.DeviceReadDepths(rect,outputDepths);
                    Check(outputDepths.Count()==1 && std::abs(outputDepths[0]-.5f)<1e-5f,"Scaled output picking depth mismatch");
                    gx.XformSetViewport(.4f,.6f,.4f,.6f,0,1);
                    gx.RsSet(GxRs_PixelShader,static_cast<void*>(&pixelShader));
                    const float uiColour[]={200.f/255,20.f/255,10.f/255,1};
                    gx.ShaderConstantsSet(GxSh_Pixel,0,uiColour,1); gx.RsSet(GxRs_DepthTest,0);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    Pixel(gx,160,120,CImVector(10,20,200,255));
                    Pixel(gx,2,2,CImVector(9,17,25,255));
                    gx.RsSet(GxRs_DepthTest,1); gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
                    CiRect full; full.minX=0; full.maxX=320; full.minY=0; full.maxY=240;
                    TSGrowableArray<CImVector> output; gx.DeviceReadPixels(full,output);
                    Check(output.Count()==320*240,"Scaled full output readback size mismatch");
                    const auto world=GxVulkan::WorldViewportRect({320,240},.17f,.83f,.22f,.78f);
                    for(int y=0;y<240;++y) for(int x=0;x<320;++x) {
                        const auto pixel=output[y*320+x];
                        if(x<world.offset.x || y<world.offset.y || x>=world.offset.x+int(world.extent.width) ||
                            y>=world.offset.y+int(world.extent.height))
                            Check(pixel.r==25 && pixel.g==17 && pixel.b==9 && pixel.a==255,"Scaled composite changed outside-viewport pixels");
                        if(x>=128 && x<192 && y>=96 && y<144)
                            Check(pixel.r==200 && pixel.g==20 && pixel.b==10 && pixel.a==255,"Native UI was filtered or scaled");
                    }
                    gx.TexDestroy(screen);
                    gx.ScenePresent();
                    // Identical requests each frame must preserve reconstruction.
                    Check(gx.SceneSetRenderScale(scale),"Unchanged scene scale rejected");
                    if(frame==2) gx.SceneInvalidateHistory();
                }
            }
            // Measure the actual LIVE output-grid path, not only a standalone
            // shader. Supersampled truth for a subpixel edge is analytic coverage.
            for(float scale:{.5f,.75f}) {
                double errors[2]{};
                for(int temporal=0;temporal<2;++temporal) {
                    gx.SceneSetRenderScale(scale); gx.SceneInvalidateHistory();
                    for(int frame=0;frame<96;++frame) {
                        gx.SceneSetRenderScale(scale);
                        gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,CImVector(9,17,25,255));
                        gx.XformSetViewport(.25f,.75f,.25f,.75f,0,1);
                        Check(gx.SceneBeginWorld(temporal!=0,21),"Live quality scene begin failed");
                        gx.SceneSetMotionOwner(&owner,true);
                        batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                        gx.ScenePauseWorldMotion(); gx.SceneEndWorld();
                        if(frame>=64) {
                            CiRect rect; rect.minX=154; rect.maxX=167; rect.minY=120; rect.maxY=121;
                            TSGrowableArray<CImVector> pixels; gx.DeviceReadPixels(rect,pixels);
                            Check(pixels.Count()==13,"Live quality native readback size mismatch");
                            for(unsigned i=0;i<pixels.Count();++i) {
                                const double truth=std::clamp(160.35-double(154+i),0.,1.);
                                const double d=pixels[i].r/255.-truth; errors[temporal]+=d*d;
                            }
                        }
                        gx.ScenePresent();
                    }
                }
                std::cout << "LIVE scale=" << scale << " temporal edge MSE=" << errors[1]/(32*13)
                    << " spatial edge MSE=" << errors[0]/(32*13) << '\n';
                Check(errors[1]<errors[0]*.95,"Live temporal reconstruction did not improve spatial edge error");
            }
            // Extent, camera identity, AA and scale transitions must discard the
            // previous red/blue scene rather than resurrect stale history.
            for(int transition=0;transition<6;++transition) {
                format.m_size=transition%2 ? C2iVector{400,300} : C2iVector{320,240};
                Check(gx.DeviceSetFormat(format)!=0,"Scaled resize failed");
                gx.SceneSetRenderScale(transition%2 ? .75f : .5f);
                gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,CImVector(9,17,25,255));
                gx.XformSetViewport(.2f,.8f,.2f,.8f,0,1);
                Check(gx.SceneBeginWorld(transition%3!=0,100+transition),"Scaled reset begin failed");
                gx.SceneSetMotionOwner(nullptr,false);
                gx.RsSet(GxRs_PixelShader,static_cast<void*>(&pixelShader));
                const float colour[]={.2f,.4f,.6f,1}; gx.ShaderConstantsSet(GxSh_Pixel,0,colour,1);
                batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                gx.ScenePauseWorldMotion(); gx.SceneEndWorld();
                Pixel(gx,format.m_size.x/2,format.m_size.y/2,CImVector(153,102,51,255));
                Pixel(gx,2,2,CImVector(9,17,25,255));
                gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr)); gx.ScenePresent();
            }
            gx.SceneSetRenderScale(1.f); gx.BufDestroy(buffer); gx.PoolDestroy(pool);
            gx.DeviceDestroy(); SDL_Quit();
            std::cout << "Live scale: native UI/depth/readback, partial viewport, FFX, resets and reconstruction passed\n";
            return 0;
        }
        for (int frame=0; frame<12; ++frame) {
            if (frame==3) {
                for (auto& vertex : vertices) vertex.colour=0xFF20A0E0;
                gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
            }
            if (frame==6) { format.m_size={400,300}; Check(gx.DeviceSetFormat(format)!=0,"Live resize failed"); }
            gx.XformSetViewport(0,1,0,1,0,1);
            gx.SceneClear(3,CImVector(9,17,25,255));
            gx.XformSetViewport(.25f,.75f,.25f,.75f,0,1);
            Check(gx.SceneBeginWorld(true,1),"Live temporal begin failed");
            gx.SceneSetMotionOwner(&owner,true);
            batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
            gx.SceneSetMotionOwner(nullptr,false);
            if (frame==2) { // Splitting a world command buffer must not promote poses early.
                CiRect rect; rect.minX=160; rect.maxX=161; rect.minY=120; rect.maxY=121;
                TSGrowableArray<float> depths; gx.DeviceReadDepths(rect,depths);
                Check(depths.Count()==1 && std::abs(depths[0]-.5f)<.00001f,"Live world depth mismatch");
            }
            gx.ScenePauseWorldMotion();
            if (frame>=3) { // FFX colour-only draw must use a single-target pipeline.
                batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
            }
            gx.SceneEndWorld();
            Pixel(gx,2,2,CImVector(9,17,25,255));
            if (frame<2) Pixel(gx,160,120,CImVector(0,0,0,255));
            else if (frame==2) Pixel(gx,160,120,CImVector(26,26,26,255)); // .1 fresh white + .9 submitted black history.
            else Pixel(gx,frame<6 ? 160 : 200,frame<6 ? 120 : 150,CImVector(224,160,32,255));
            // Native UI drawn over the world AFTER history production must not
            // contaminate next frame's measured black/white edge accumulation.
            gx.XformSetViewport(.4f,.6f,.4f,.6f,0,1);
            gx.RsSet(GxRs_PixelShader,static_cast<void*>(&pixelShader));
            const float uiColour[]={200.f/255,20.f/255,10.f/255,1};
            gx.ShaderConstantsSet(GxSh_Pixel,0,uiColour,1);
            gx.RsSet(GxRs_DepthTest,0);
            batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
            Pixel(gx,frame<6 ? 160 : 200,frame<6 ? 120 : 150,CImVector(10,20,200,255));
            gx.RsSet(GxRs_DepthTest,1);
            gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
            gx.XformSetViewport(0,1,0,1,0,1);
            if (frame==3) gx.SceneForgetMotionOwner(&owner);
            if (frame==4) gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
            if (frame==8) gx.SceneInvalidateHistory();
            gx.ScenePresent();
        }
        // Recreate the known edge sequence, but disallow pose reuse in turn.
        for (int rejection=0; rejection<7; ++rejection) {
            gx.SceneInvalidateHistory();
            for (unsigned i=0; i<vertices.size(); ++i) vertices[i].colour=i<6 ? 0xFFFFFFFF : 0xFF000000;
            gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
            for (int frame=0; frame<3; ++frame) {
                // Use the original dimensions so the analytic pixel boundary is identical.
                if (frame==0) { format.m_size={320,240}; gx.DeviceSetFormat(format); }
                if (rejection==3) gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
                if (rejection==4) gx.SceneForgetMotionOwner(&owner);
                gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,CImVector(9,17,25,255));
                gx.XformSetViewport(.25f,.75f,.25f,.75f,0,1);
                Check(gx.SceneBeginWorld(true,1),"Rejected-owner live begin failed");
                if (rejection==6) {
                    // Seed matching underlying depth; the overlay itself never writes depth.
                    gx.SceneSetMotionOwner(nullptr,false);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    gx.RsSet(GxRs_DepthTest,0);
                }
                gx.SceneSetMotionOwner(rejection==0 ? nullptr : &owner,rejection!=1);
                for (int duplicate=0; duplicate<(rejection==2 ? 2 : 1); ++duplicate) {
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                }
                if (rejection==5 && frame==2) {
                    // First acquire valid history, then discover a duplicate whose
                    // clipped coverage cannot overwrite the first draw's motion.
                    C44Matrix moved; moved.Translate(C3Vector(3,0,0));
                    gx.XformPushAndSet(GxXform_World,moved);
                    batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
                    GxXformPop(GxXform_World);
                }
                gx.RsSet(GxRs_DepthTest,1);
                gx.SceneSetMotionOwner(nullptr,false); gx.SceneEndWorld();
                if (frame==2) {
                    const uint8_t fresh = rejection==5 ? 255 : 0;
                    Pixel(gx,160,120,CImVector(fresh,fresh,fresh,255));
                }
                gx.ScenePresent();
            }
        }
        // Actual CGxShader objects exercise IShaderCreate + lazy live variants,
        // descriptor snapshots and packed registers, not just detached compilation.
        for (int mixed=0; mixed<2; ++mixed) for (int frame=0; frame<3; ++frame) {
            gx.XformSetViewport(0,1,0,1,0,1); gx.SceneClear(3,CImVector(9,17,25,255));
            gx.XformSetViewport(.25f,.75f,.25f,.75f,.2f,.8f);
            Check(gx.SceneBeginWorld(true,2),"Live recovered begin failed");
            gx.RsSet(GxRs_VertexShader,mixed ? nullptr : static_cast<void*>(&vertexShader));
            gx.RsSet(GxRs_PixelShader,static_cast<void*>(&pixelShader));
            const float vertexConstants[]={0,0,.5f,0}, pixelConstants[]={.2f,.4f,.6f,.8f};
            gx.ShaderConstantsSet(GxSh_Vertex,0,vertexConstants,1);
            gx.ShaderConstantsSet(GxSh_Pixel,0,pixelConstants,1);
            gx.SceneSetMotionOwner(&owner,true);
            batch.m_start=0; gx.Draw(&batch,0); batch.m_start=6; gx.Draw(&batch,0);
            gx.SceneSetMotionOwner(nullptr,false); gx.SceneEndWorld();
            Pixel(gx,160,120,CImVector(153,102,51,204));
            gx.ScenePresent();
        }
        gx.RsSet(GxRs_VertexShader,static_cast<void*>(nullptr)); gx.RsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
        gx.SceneInvalidateHistory();
        gx.BufDestroy(buffer); gx.PoolDestroy(pool);
        gx.SceneClear(3,CImVector(9,17,25,255));
        const char* engineering=std::getenv("WOWDECOMP_VULKAN_WORLD_TARGET");
        const bool expectedEngineering=engineering && std::strcmp(engineering,"1")==0;
        Check(gx.SceneBeginWorld(false,1)==expectedEngineering,"Raster-off/world-target switch mismatch");
        gx.SceneEndWorld();
        gx.ScenePresent();
        gx.DeviceDestroy(); SDL_Quit();
        std::cout << "Live backend synthetic: measured .9 history accumulation, TAA fixed/recovered/mixed world passes, readback splits, unknown/deforming/duplicate/mutated/forgotten-owner rejection, reset, resize, viewport-only composite and raster-off passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
