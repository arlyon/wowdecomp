#include "Engine/Source/Gx/CGxDeviceVulkan/SpirvNonUniform.h"
#include <fstream>
#include <filesystem>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Expected one generated SPIR-V file");
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open generated SPIR-V");
    const std::streamoff size=input.tellg();if(size<20||size%4)throw std::runtime_error("Invalid generated SPIR-V size");
    std::vector<uint32_t> words(size_t(size)/4);input.seekg(0);input.read(reinterpret_cast<char*>(words.data()),size);
    if(!input)throw std::runtime_error("Cannot read generated SPIR-V");input.close();
    auto qualified=GxVulkan::QualifyNonUniformResourceLoads(words);
    if(qualified!=words){const auto temporary=std::filesystem::path(std::string(argv[1])+".qualified");
        {std::ofstream output(temporary,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(qualified.data()),qualified.size()*4);output.close();
            if(!output)throw std::runtime_error("Cannot write qualified SPIR-V");}
        std::filesystem::rename(temporary,argv[1]);}
    return 0;
}catch(const std::exception& error){std::cerr<<"SPIR-V descriptor qualification: "<<error.what()<<'\n';return 1;}}
