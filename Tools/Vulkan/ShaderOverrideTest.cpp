#include "Engine/Source/Gx/CGxDeviceVulkan/ShaderOverride.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/SpirvNonUniform.h"
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
#include "Engine/Source/Gx/CGxDeviceVulkan/Slang.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>
using namespace GxVulkan;
namespace fs=std::filesystem;
namespace {
void Check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
template<class F> void Reject(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed,"Invalid override was accepted");}
void Write(const fs::path& p,const std::string& value){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<value;Check(bool(f),"fixture source write failed");}
struct Scratch {
    fs::path path=fs::current_path()/("shader-override-fixture-"+std::to_string(getpid()));
    Scratch(){Check(!fs::exists(path),"refuse existing fixture directory");fs::create_directory(path);}
    ~Scratch(){std::error_code error;fs::remove_all(path,error);}
};
}
int main(){try {
    const std::vector<uint32_t> rawModule={0x07230203,0x00010500,0,10,0,
        (3u<<16)|71,4,5300,(3u<<16)|71,6,5300,
        (3u<<16)|22,1,32,(2u<<16)|26,2,(4u<<16)|32,3,0,2,
        (4u<<16)|61,2,5,4,(4u<<16)|61,1,7,6};
    const auto qualified=QualifyNonUniformResourceLoads(rawModule);
    Check(qualified.size()==rawModule.size()+3,"descriptor qualification missed resource load or tainted arithmetic");
    Check(QualifyNonUniformResourceLoads(qualified)==qualified,"descriptor qualification is not idempotent");
    Check(QualifyNonUniformResourceLoads({0x07230203,0x00010500,0,1,0})==std::vector<uint32_t>({0x07230203,0x00010500,0,1,0}),"empty qualification changed module");
    Reject([&]{QualifyNonUniformResourceLoads({});});
    auto broken=rawModule;broken[5]=0;Reject([&]{QualifyNonUniformResourceLoads(broken);});
    broken=rawModule;broken.pop_back();Reject([&]{QualifyNonUniformResourceLoads(broken);});
    broken=rawModule;broken[3]=5;Reject([&]{QualifyNonUniformResourceLoads(broken);});
    Scratch scratch;const auto root=scratch.path/"root";
    const uint32_t version=0xffff0300;
    const auto key=IdentifyShader(ShaderStage::Pixel,"Terrain:17",&version,sizeof(version));
    Check(key && key->name=="Terrain" && key->profile=="ps_3_0" && key->permutation==17,"original Gx identity/profile/permutation changed");
    Check(!IdentifyShader(ShaderStage::Pixel,nullptr,&version,sizeof(version)),"anonymous compatibility program acquired a false identity");
    Reject([&]{IdentifyShader(ShaderStage::Vertex,"Terrain",&version,sizeof(version));});
    Reject([&]{IdentifyShader(ShaderStage::Pixel,"Terrain:4294967296",&version,sizeof(version));});
    Reject([&]{IdentifyShader(ShaderStage::Pixel,"Terrain:-1",&version,sizeof(version));});
    ShaderIdentity id{ShaderStage::Pixel,"Terrain","ps_3_0",17};
    const auto file=root/"pixel/ps_3_0/Terrain/17.slang";
    Check(!ResolveShaderOverride(root,id),"missing root did not permit generation");
    Write(file,"// first maintained source\n");
    auto first=ResolveShaderOverride(root,id);Check(first && first->source=="// first maintained source\n","readable identity resolution failed");
    Check(first->filename==fs::canonical(file),"reported source identity is not canonical-root based");
    auto variant=id;variant.permutation=18;Check(!ResolveShaderOverride(root,variant),"permutation path collision");
    variant=id;variant.profile="ps_2_0";Check(!ResolveShaderOverride(root,variant),"profile path collision");
    variant=id;variant.stage=ShaderStage::Vertex;Check(!ResolveShaderOverride(root,variant),"stage path collision");
    variant=id;variant.name="../outside";Reject([&]{ResolveShaderOverride(root,variant);});
    variant=id;variant.profile="/absolute";Reject([&]{ResolveShaderOverride(root,variant);});
    variant=id;variant.stage=static_cast<ShaderStage>(99);Reject([&]{ResolveShaderOverride(root,variant);});
    Write(file,"// edited maintained source\n");
    Check(first->files->Read(first->filename)==first->source,"a generation mixed edited source bytes");
    auto edited=ResolveShaderOverride(root,id);Check(edited && edited->source!="// first maintained source\n","new resolve concealed edited source");
    Write(root/"shared/common.slang","// shared\n");
    Check(edited->files->Read("shared/common.slang")=="// shared\n","confined include lookup failed");
    Write(scratch.path/"outside.slang","// outside\n");
    Reject([&]{edited->files->Read("../outside.slang");});
    Reject([&]{edited->files->Read(scratch.path/"outside.slang");});
    fs::create_directory_symlink(scratch.path,root/"escape");
    Reject([&]{edited->files->Read("escape/outside.slang");});
#if defined(WOWDECOMP_TEST_SLANG_COMPILER)
    SlangCompiler compiler;
    const auto compile=[&] {
        auto selected=ResolveShaderOverride(root,id);Check(bool(selected),"present compiler source disappeared");
        return compiler.Compile(selected->source,false,{selected->files,selected->filename.generic_string()});
    };
    Write(root/"shared/common.slang","float4 Colour(){return float4(1,0,0,1);}\n");
    Write(file,"#include \"shared/common.slang\"\nfloat4 main():SV_Target0{return Colour();}\n");
    auto snapshot=ResolveShaderOverride(root,id);Check(bool(snapshot),"maintained source resolution failed");
    const ShaderCompileContext context{snapshot->files,snapshot->filename.generic_string()};
    const auto original=compiler.Compile(snapshot->source,false,context);Check(!original.empty(),"confined include compilation failed");
    Write(root/"shared/common.slang","float4 Colour(){return float4(0,0,1,1);}\n");
    Check(compiler.Compile(snapshot->source,false,context)==original,"one generation mixed edited include bytes");
    Check(compile()!=original,"fresh source generation concealed an edited include");
    Write(file,"#include \""+(scratch.path/"outside.slang").generic_string()+"\"\nfloat4 main():SV_Target0{return 1;}\n");
    Reject(compile);
    Write(file,"#include \"escape/outside.slang\"\nfloat4 main():SV_Target0{return 1;}\n");Reject(compile);
    Write(file,"present but invalid shader");Reject(compile);
    const auto maintained=[&](const std::string& text) {
        Write(file,text);auto selected=ResolveShaderOverride(root,id);Check(bool(selected),"maintained source missing");
        return compiler.CompileMaintained(*selected);
    };
    const std::string registers="[[vk::binding(0,3)]] cbuffer Registers {float4 values[256];};\n";
    const std::string alpha="[[vk::binding(1,3)]] cbuffer Alpha {float4 threshold;};\n";
    const auto pixelProgram=maintained(registers+alpha+
        "[[vk::binding(0,2)]] Sampler2D<float4> image;\n"
        "float4 main():SV_Target0 {float4 c=values[1]*image.Sample(float2(.5,.5));clip(c.a-threshold.x);return c;}\n");
    Check(pixelProgram.bindings.registers.size()==1 && pixelProgram.bindings.registers[0].count==256 &&
        pixelProgram.bindings.samplers==std::vector<int>{0},"maintained bindings require bytecode frontend metadata");
    Reject([&]{maintained(alpha+"float4 main():SV_Target1 {clip(1-threshold.x);return 1;}");});
    Reject([&]{maintained("float4 main():SV_Target0{return 1;}");});
    Reject([&]{maintained("[[vk::binding(0,3)]] cbuffer Short {float4 values[2];};\n"+alpha+
        "float4 main():SV_Target0{clip(1-threshold.x);return values[0];}");});
    Reject([&]{maintained("[[vk::binding(0,5)]] cbuffer WrongSet {float4 values[256];};\n"+alpha+
        "float4 main():SV_Target0{clip(1-threshold.x);return values[0];}");});
    Reject([&]{maintained(alpha+"[[vk::binding(17,2)]] Sampler2D<float4> image;\n"
        "float4 main():SV_Target0{clip(1-threshold.x);return image.Sample(float2(0,0));}");});
    for(const auto* shared:{"ShadowCoverage.slang","DirectionalLighting.slang"})
        fs::copy_file(fs::path(WOWDECOMP_TEST_SHARED_SHADERS)/shared,root/"shared"/shared);
    const std::string directionalSource=registers+alpha+
        "#include \"shared/DirectionalLighting.slang\"\n"
        "struct Result {float4 colour:SV_Target0;float4 classic:SV_Target2;};\nResult main(){float v=1;\n"
        "#if GX_DIRECTIONAL_SHADOWS\n v=GxWorldSunVisibility(float3(0,0,0));\n#endif\n"
        "float3 c=GxComposeLighting(values[0].xyz,values[1].xyz,values[2].xyz,values[3].xyz,values[4].xyz,v);clip(1-threshold.x);Result result;result.colour=float4(c,1);result.classic=values[5];return result;}";
    Write(file,directionalSource);auto directional=ResolveShaderOverride(root,id);
    const auto ordinary=compiler.CompileMaintained(*directional);
    Check(!ordinary.bindings.directionalVisibility&&!ordinary.bindings.shadowFrame,"raster source unexpectedly requires ray capabilities");
    const auto inlineProgram=compiler.CompileMaintained(*directional,MaintainedShaderVariant::Directional);
    Check(inlineProgram.bindings.directionalVisibility&&inlineProgram.bindings.shadowFrame,"explicit inline directional interface was not reflected");
    Reject([&]{compiler.CompileMaintained(*directional,static_cast<MaintainedShaderVariant>(99));});
    Write(file,registers+alpha+"#include \"shared/DirectionalLighting.slang\"\nfloat4 main():SV_Target0{clip(1-threshold.x);return float4(GxWorldSunVisibility(float3(0,0,0)),0,0,1);}");
    auto missingClassic=ResolveShaderOverride(root,id);Reject([&]{compiler.CompileMaintained(*missingClassic,MaintainedShaderVariant::Directional);});
    Write(file,registers+alpha+"float4 main():SV_Target0{clip(1-threshold.x);return values[0];}");
    auto missingDirectional=ResolveShaderOverride(root,id);
    Reject([&]{compiler.CompileMaintained(*missingDirectional,MaintainedShaderVariant::Directional);});
    Write(file,registers+alpha+"#include \"shared/ShadowCoverage.slang\"\nfloat4 main():SV_Target0{clip(1-threshold.x);return values[0];}");
    auto unguarded=ResolveShaderOverride(root,id);Reject([&]{compiler.CompileMaintained(*unguarded);});
    ShaderIdentity vertexId{ShaderStage::Vertex,"Terrain","vs_3_0",0};
    const auto vertexFile=root/"vertex/vs_3_0/Terrain/0.slang";
    Write(vertexFile,"[[vk::binding(0,1)]] cbuffer Registers {float4 values[256];};\n"
        "struct Input {[[vk::location(0)]] float3 position:POSITION;[[vk::location(6)]] float2 uv:TEXCOORD0;};\n"
        "struct Output {float4 position:SV_Position;[[vk::location(2)]] float2 uv:TEXCOORD0;};\n"
        "Output main(Input input){Output o;o.position=float4(input.position,1)+values[0];o.uv=input.uv;return o;}");
    auto selectedVertex=ResolveShaderOverride(root,vertexId);Check(bool(selectedVertex),"vertex source missing");
    Check(compiler.CompileMaintained(*selectedVertex).bindings.attributes==std::vector<int>({0,6}),"compiled vertex layout locations changed");
    Write(vertexFile,"[[vk::binding(0,1)]] cbuffer Registers {float4 values[256];};\n"
        "#include \"shared/DirectionalLighting.slang\"\n"
        "float4 main([[vk::location(0)]] float3 p:POSITION):SV_Position{\n"
        "#if GX_DIRECTIONAL_SHADOWS\np=GxShadowWorldPosition(p);\n#endif\nreturn float4(p,1)+values[0];}");
    auto inlineVertex=ResolveShaderOverride(root,vertexId);
    Check(compiler.CompileMaintained(*inlineVertex,MaintainedShaderVariant::Directional).bindings.shadowFrame,"directional vertex frame interface missing");
    // Prove that neither fixed arrays nor a padded-but-wrong triangle record
    // acquire runtime coverage permission merely by using the right binding.
    std::ifstream sharedInput(fs::path(WOWDECOMP_TEST_SHARED_SHADERS)/"ShadowCoverage.slang");
    const std::string sharedText((std::istreambuf_iterator<char>(sharedInput)),std::istreambuf_iterator<char>());
    auto malformed=sharedText;auto editAt=malformed.find("gxShadowTextures[]");Check(editAt!=std::string::npos,"texture fixture anchor missing");
    malformed.replace(editAt,std::string("gxShadowTextures[]").size(),"gxShadowTextures[1]");
    Write(root/"shared/ShadowCoverage.slang",malformed);Write(file,directionalSource);
    auto fixedArray=ResolveShaderOverride(root,id);Reject([&]{compiler.CompileMaintained(*fixedArray,MaintainedShaderVariant::Directional);});
    malformed=sharedText;editAt=malformed.find("uint4 flags");Check(editAt!=std::string::npos,"coverage fixture anchor missing");
    malformed.replace(editAt,std::string("uint4 flags").size(),"uint3 flags");
    Write(root/"shared/ShadowCoverage.slang",malformed);auto wrongTriangle=ResolveShaderOverride(root,id);
    Reject([&]{compiler.CompileMaintained(*wrongTriangle,MaintainedShaderVariant::Directional);});
    Write(root/"shared/ShadowCoverage.slang",sharedText);
    Write(vertexFile,"[[vk::binding(4,0)]] Sampler2D<float4> image;\n"
        "float4 main([[vk::location(0)]] float3 p:POSITION):SV_Position{return float4(p,1)+image.SampleLevel(float2(0,0),0);}");
    auto excessiveVertexSampler=ResolveShaderOverride(root,vertexId);
    Reject([&]{compiler.CompileMaintained(*excessiveVertexSampler);});
#endif
    fs::remove(file);fs::create_symlink(scratch.path/"outside.slang",file);
    Reject([&]{ResolveShaderOverride(root,id);});
    fs::remove(file);fs::create_symlink(scratch.path/"missing.slang",file);
    Reject([&]{ResolveShaderOverride(root,id);});
    fs::remove(file);fs::create_directory(file);Reject([&]{ResolveShaderOverride(root,id);});fs::remove(file);
    Write(file,"");Reject([&]{ResolveShaderOverride(root,id);});
    Write(file,std::string("text\0hidden",11));Reject([&]{ResolveShaderOverride(root,id);});
    Write(file,std::string(4*1024*1024+1,' '));Reject([&]{ResolveShaderOverride(root,id);});
    Write(file,"// inaccessible\n");chmod(file.c_str(),0);
    const bool inaccessible=access(file.c_str(),R_OK)!=0;
    if(inaccessible)Reject([&]{ResolveShaderOverride(root,id);});
    else Check(bool(ResolveShaderOverride(root,id)),"privileged readable file unexpectedly failed");
    chmod(file.c_str(),0600);
    // The snapshot bound includes entry and all includes, not merely one file.
    ShaderSourceFiles bounded(root);const std::string large(4*1024*1024,' ');
    for(unsigned i=0;i<8;++i){const auto name="budget/"+std::to_string(i)+".slang";Write(root/name,large);Check(bool(bounded.Read(name)),"legal include budget refused");}
    Write(root/"budget/extra.slang","x");Reject([&]{bounded.Read("budget/extra.slang");});
    fs::remove(file);Check(mkfifo(file.c_str(),0600)==0,"fixture FIFO creation failed");
    Reject([&]{ResolveShaderOverride(root,id);});
    std::cout<<"PASS override identity, missing/present errors, immutable generations and confined nonblocking entry/includes; unreadable case="<<inaccessible<<"\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL override filesystem: "<<e.what()<<'\n';return 1;}}
