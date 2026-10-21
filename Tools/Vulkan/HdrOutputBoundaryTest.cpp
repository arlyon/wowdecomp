#include "Engine/Source/Gx/CGxDeviceVulkan/Shader.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Slang.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <iostream>
#include <set>
namespace {
void Check(bool b,const char* s) {if(!b)throw std::runtime_error(s);}
std::vector<uint8_t> Record(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "Cannot open supplied shader container");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    const auto word = [&](size_t offset) {
        Check(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated supplied shader container");
        uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
    };
    Check(word(0) == 0x47585348 && word(4) == 0x10003 && word(8) > 0, "Invalid supplied shader container");
    const auto size = word(24);
    Check(bytes.size() >= 28 && size <= bytes.size() - 28, "Truncated supplied shader record");
    return {bytes.begin() + 28, bytes.begin() + 28 + size};
}
}
int main(int argc,char** argv) {
    try {
        GxVulkan::SlangCompiler compiler;unsigned passed=0;
        auto run=[&](const void* bytes,size_t size) {
            const auto recovered=GxVulkan::RecoverSlangShader(bytes,size);
            for(bool decode:{false,true}) {
                const auto wrapped=GxVulkan::ShaderOutputBoundary(recovered.source,decode);
                Check(wrapped.find("output.gx_classicColour=output.")!=std::string::npos,"missing original C0 companion");
                Check(wrapped.find("SV_Target2")!=std::string::npos,"missing non-RT companion ABI");
                compiler.Compile(wrapped,false);
            }
            ++passed;
        };
        const uint32_t ps[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};run(ps,sizeof(ps));
        bool rejected=false;try {GxVulkan::ShaderOutputBoundary("float4 main() : SV_Target0 { return 0; }");}catch(const std::exception&){rejected=true;}
        Check(rejected,"unsupported output ABI admitted");
        if(argc==2) {
            const std::filesystem::path root=argv[1];std::set<std::vector<uint8_t>> unique;
            for(const char* profile:{"ps_2_0","ps_3_0"}) for(const auto& entry:std::filesystem::directory_iterator(root/"pixel"/profile)) {
                if(entry.path().extension()!=".bls")continue;
                auto bytes=Record(entry.path());
                if(unique.insert(bytes).second)run(bytes.data(),bytes.size());
            }
            Check(unique.size()>10,"actual recovered-program supplement was empty");
        }
        std::cout<<"PASS: generic C0 decode/raw-linear + original-C0 companion compiled for "<<passed<<" distinct SM2/3 recovered outputs\n";return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
