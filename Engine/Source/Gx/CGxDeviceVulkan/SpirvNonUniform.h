#ifndef GX_VULKAN_SPIRV_NON_UNIFORM_H
#define GX_VULKAN_SPIRV_NON_UNIFORM_H
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>
namespace GxVulkan {
// The pinned Slang backend qualifies a divergent descriptor's access-chain but
// omits NonUniform on its loaded resource. RADV can then select a wave-uniform
// sampler (e.g. repeat instead of clamp). Qualify that resource, not arithmetic
// or material meaning. Shared by runtime compilation and authored kernel builds.
inline std::vector<uint32_t> QualifyNonUniformResourceLoads(const std::vector<uint32_t>& code) {
    if(code.size()<5||code[0]!=0x07230203)throw std::runtime_error("Invalid SPIR-V header");
    std::set<uint32_t> nonuniform,resources,missing;
    size_t annotationEnd=code.size();
    for(size_t i=5;i<code.size();){const auto length=code[i]>>16,op=code[i]&0xffff;
        if(!length||length>code.size()-i)throw std::runtime_error("Invalid SPIR-V instruction length");
        if(op==71&&length>=3&&code[i+2]==5300)nonuniform.insert(code[i+1]);
        if(op==25||op==26||op==27||op==5341){if(length<2)throw std::runtime_error("Invalid SPIR-V resource type");resources.insert(code[i+1]);}
        if(annotationEnd==code.size()&&op>=19&&op<=39)annotationEnd=i;
        i+=length;
    }
    for(size_t i=5;i<code.size();){const auto length=code[i]>>16,op=code[i]&0xffff;
        if(op==61){if(length<4)throw std::runtime_error("Invalid SPIR-V load");
            if(resources.count(code[i+1])&&nonuniform.count(code[i+3])&&!nonuniform.count(code[i+2])){
                if(!code[i+2]||code[i+2]>=code[3])throw std::runtime_error("Invalid SPIR-V resource id");
                missing.insert(code[i+2]);
            }
        }
        i+=length;
    }
    if(missing.empty())return code;
    if(annotationEnd==code.size())throw std::runtime_error("Missing SPIR-V type section");
    std::vector<uint32_t> result;result.reserve(code.size()+missing.size()*3);
    result.insert(result.end(),code.begin(),code.begin()+annotationEnd);
    for(auto id:missing)result.insert(result.end(),{(3u<<16)|71u,id,5300u});
    result.insert(result.end(),code.begin()+annotationEnd,code.end());return result;
}
}
#endif
