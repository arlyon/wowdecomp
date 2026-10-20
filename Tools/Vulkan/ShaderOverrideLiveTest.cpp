// Asset-free real native loader/variant/cache oracle. Handwritten source only.
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/HdrToneMapping.h"
#include "Engine/Source/Gx/Gx.h"
#include <SDL.h>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unistd.h>
namespace fs=std::filesystem;
namespace {
void Check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
struct Scratch {
    fs::path path=fs::current_path()/("shader-override-live-"+std::to_string(getpid()));
    std::optional<std::string> previous;
    Scratch(){
        if(const auto* value=std::getenv("WOWDECOMP_SHADER_ROOT"))previous=value;
        Check(!fs::exists(path),"refuse existing live fixture directory");fs::create_directory(path);
        Check(setenv("WOWDECOMP_SHADER_ROOT",path.c_str(),1)==0,"fixture root setup");
    }
    ~Scratch(){if(previous)setenv("WOWDECOMP_SHADER_ROOT",previous->c_str(),1);else unsetenv("WOWDECOMP_SHADER_ROOT");std::error_code e;fs::remove_all(path,e);}
};
void Write(const fs::path& p,const std::string& value){fs::create_directories(p.parent_path());std::ofstream f(p);f<<value;Check(bool(f),"source write failed");}
void Program(CGxShader& s,EGxShTarget stage,const char* key,const uint32_t* words,size_t bytes){s.m_target=stage;s.m_key=key;s.m_code.SetCount(bytes);std::memcpy(s.m_code.Ptr(),words,bytes);}
CImVector Read(CGxDeviceVulkan& gx,int x=160,int y=120){CiRect r;r.minX=x;r.maxX=x+1;r.minY=y;r.maxY=y+1;TSGrowableArray<CImVector> p;gx.DeviceReadPixels(r,p);Check(p.Count()==1,"readback missing");return p[0];}
const char* vertexSource=R"(
[[vk::binding(0, 1)]] cbuffer VertexRegisters
{
    float4 registers[256];
};
struct main_Input
{
    [[vk::location(0)]] float3 position : POSITION;
};
struct main_Output
{
    float4 clip : SV_Position;
};
main_Output main(main_Input input)
{
    main_Output output = (main_Output)0;
    output.clip = float4(input.position, 1);
    output.clip.xy += registers[0].xy;
    return output;
}
)";
std::string PixelSource(bool blue){return std::string(R"(
[[vk::binding(1, 3)]] cbuffer AlphaTest
{
    float4 alphaTest;
};
struct main_Output
{
    float4 m_oC0 : SV_Target0;
};
main_Output main()
{
    main_Output output = (main_Output)0;
    output.m_oC0 = )")+(blue?"float4(0,0,1,1);":"float4(0,1,0,1);")+R"(
    clip(output.m_oC0.a - alphaTest.x);
    return output;
}
)";}
}
int main(){try{
    Scratch scratch;
    const auto vf=scratch.path/"vertex/vs_2_0/Fixture/3.slang",pf=scratch.path/"pixel/ps_2_0/Fixture/7.slang";
    Write(vf,vertexSource);Write(pf,PixelSource(false));
    Check(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
    CGxDeviceVulkan gx;g_theGxDevicePtr=&gx;
    CGxFormat format;format.m_window=1;format.m_maximize=0;format.m_size={320,240};
    Check(gx.DeviceCreate(static_cast<GxWindowProc>(nullptr),format)!=0,"native device creation");gx.m_cursorVisible=0;
    for(auto state:{GxRs_Culling,GxRs_DepthTest,GxRs_DepthWrite,GxRs_Fog,GxRs_BlendingMode})gx.RsSet(state,0);
    gx.RsSet(GxRs_ColorWrite,15);
    C44Matrix identity;gx.XformPushAndSet(GxXform_World,identity);gx.XformSetView(identity);gx.XformSetProjection(identity);
    struct Vertex{float x,y,z;};
    auto* pool=gx.PoolCreate(GxPoolTarget_Vertex,GxPoolUsage_Static,6*sizeof(Vertex),GxPoolHintBit_Unk3,"override fixture");
    auto* buffer=gx.BufCreate(pool,sizeof(Vertex),6,0);
    const std::array<Vertex,6> vertices{{{-1,-1,0},{1,-1,0},{-1,1,0},{-1,1,0},{1,-1,0},{1,1,0}}};
    gx.BufData(buffer,vertices.data(),sizeof(vertices),0);
    const CGxVertexAttrib attr{GxVA_Position,4,0,sizeof(Vertex)};gx.PrimVertexFormat(buffer,&attr,1);gx.PrimVertexMask(1u<<GxVA_Position);
    CGxBatch batch{};batch.m_primType=GxPrim_Triangles;batch.m_count=6;
    CGxShader vs,ps;
    // This body cannot be recovered: successful maintained VS/AA draws prove
    // that selecting source does not invoke the bytecode frontend on the side.
    const uint32_t invalidVertex[]={0xfffe0200,0xdeadbeef,0x0000ffff};
    const uint32_t validPixel[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    Program(vs,GxSh_Vertex,"Fixture:3",invalidVertex,sizeof(invalidVertex));Program(ps,GxSh_Pixel,"Fixture:7",validPixel,sizeof(validPixel));
    gx.RsSet(GxRs_VertexShader,static_cast<void*>(&vs));gx.RsSet(GxRs_PixelShader,static_cast<void*>(&ps));
    const float zero[]={0,0,0,0},red[]={1,0,0,1};gx.ShaderConstantsSet(GxSh_Vertex,0,zero,1);gx.ShaderConstantsSet(GxSh_Pixel,0,red,1);
    unsigned completed=0;
    const auto matrix=[&](bool blue){
        for(float scale:{1.f,.5f})for(bool temporal:{false,true})for(bool hdr:{false,true})for(unsigned frame=0;frame<3;++frame){
            Check(gx.SceneSetRenderScale(scale) && gx.SceneSetHdr(hdr,0),"feature request failed");
            gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
            const bool world=gx.SceneBeginWorld(temporal,41);Check(world || (!hdr && !temporal && scale==1),"world begin failed");
            gx.SceneSetMotionOwner(&vs,true);gx.Draw(&batch,0);gx.SceneEndWorld();
            const auto pixel=Read(gx);const float expected=hdr?GxVulkan::HdrEncodeSdr(GxVulkan::HdrToneMapLinear(1,0)):1;
            Check(pixel.r<=2 && (blue?pixel.g:pixel.b)<=2 && std::abs(int(blue?pixel.b:pixel.g)-int(std::lround(expected*255)))<=2,
                "selected source was bypassed, stale, or changed through AA/HDR/scale");
            if(hdr || temporal || scale!=1){const auto info=gx.SceneWorldTestCompletedFrame();Check(bool(info[0])==hdr && bool(info[1])==temporal,"variant silently fell back");}
            gx.XformSetViewport(0,.125f,0,.125f,0,1);gx.Draw(&batch,0);const auto ui=Read(gx,10,230);
            Check(ui.r==0 && (blue?ui.b:ui.g)==255 && (blue?ui.g:ui.b)==0,"native UI lost selected source or was toned");
            gx.ScenePresent();++completed;
        }
    };
    matrix(false);
    Write(pf,"present but malformed override");gx.IShaderCreate(&ps);Check(!ps.m_valid && !ps.m_apiSpecific,"invalid override silently generated the valid red bytecode");
    Write(pf,"[[vk::binding(0,9)]] cbuffer Wrong {float4 c[256];}; float4 main():SV_Target0{return c[0];}");
    gx.IShaderCreate(&ps);Check(!ps.m_valid,"incompatible override silently generated compatibility source");
    Write(pf,PixelSource(true));gx.IShaderCreate(&ps);Check(ps.m_valid,"explicit reload did not pick up edited source");matrix(true);
    // An included implicit register block cannot silently turn previous-pose
    // evaluation into a second read of current registers. Unsupported source
    // adapter structure must fail, not claim a completed motion variant.
    std::string indirect=vertexSource;const auto blockEnd=indirect.find("};")+2;
    Write(scratch.path/"shared/registers.slang",indirect.substr(0,blockEnd));
    Write(vf,"#include \"shared/registers.slang\"\n"+indirect.substr(blockEnd));gx.IShaderCreate(&vs);Check(vs.m_valid,"indirect base override should be valid raster source");
    gx.SceneSetRenderScale(1);gx.SceneSetHdr(false,0);gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));
    Check(gx.SceneBeginWorld(true,99),"unsupported temporal oracle scope");
    bool rejectedMotion=false;
    try{gx.Draw(&batch,0);}catch(const std::runtime_error& error){rejectedMotion=std::string(error.what()).find("previous registers/motion parameters")!=std::string::npos;}
    Check(rejectedMotion,"unsupported temporal source silently reused current pose");gx.SceneEndWorld();gx.ScenePresent();
    Write(vf,vertexSource);gx.IShaderCreate(&vs);Check(vs.m_valid,"restore direct-register source");
    // Absence alone permits compatibility generation, using the same Gx draws.
    CGxShader fallback;
    Program(fallback,GxSh_Pixel,"MissingOverride:0",validPixel,sizeof(validPixel));gx.RsSet(GxRs_PixelShader,static_cast<void*>(&fallback));
    gx.SceneSetRenderScale(1);gx.SceneSetHdr(false,0);gx.XformSetViewport(0,1,0,1,0,1);gx.SceneClear(3,CImVector(0,0,0,255));gx.Draw(&batch,0);
    const auto generated=Read(gx);Check(generated.r==255 && generated.g==0 && generated.b==0,"missing override did not use generated compatibility shader");gx.ScenePresent();
    gx.BufDestroy(buffer);gx.PoolDestroy(pool);gx.DeviceDestroy();Check(gx.SceneHdrTestValidationClean(),"unexpected Vulkan diagnostics");g_theGxDevicePtr=nullptr;SDL_Quit();
    std::cout<<"PASS native override loader: "<<completed<<" selected-source frames, AA/HDR/scale/native UI, deliberate invalid/interface errors, edited-source reload and absent-only fallback\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native shader override: "<<e.what()<<'\n';return 1;}}
