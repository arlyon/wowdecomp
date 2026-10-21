#ifndef GX_VULKAN_HDR_LUT_H
#define GX_VULKAN_HDR_LUT_H
#include <array>
#include <cmath>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace GxVulkan {
inline constexpr size_t HdrLutBudget = 2u*1024u*1024u;
struct HdrLut { unsigned size=0; std::vector<std::array<float,3>> rgb; };
// Strict normalized .cube subset: red changes fastest, DOMAIN [0,1], 2..33.
// Values are display-encoded SDR RGB after tone mapping, NOT light transport.
inline HdrLut ReadHdrCube(std::istream& stream) {
    HdrLut lut; std::string line; bool data=false, domainMin=false, domainMax=false;
    char lineBuffer[4097];
    while(stream.getline(lineBuffer,sizeof(lineBuffer))) {
        line=lineBuffer;
        const auto comment=line.find('#'); if(comment!=std::string::npos) line.resize(comment);
        std::istringstream fields(line);std::string token;if(!(fields>>token)) continue;
        if(token=="TITLE" && !data) {std::string title;std::getline(fields,title);if(title.find('"')==std::string::npos) throw std::runtime_error("Invalid LUT title");continue;}
        if(token=="LUT_3D_SIZE" && !data && !lut.size) {
            if(!(fields>>lut.size) || lut.size<2 || lut.size>33) throw std::runtime_error("LUT size must be 2..33");
            lut.rgb.reserve(size_t(lut.size)*lut.size*lut.size);
        } else if((token=="DOMAIN_MIN" || token=="DOMAIN_MAX") && !data) {
            auto& seen=token=="DOMAIN_MIN"?domainMin:domainMax;if(seen) throw std::runtime_error("Duplicate LUT domain");seen=true;
            const float expected=token=="DOMAIN_MIN"?0.f:1.f;std::array<float,3> value{};
            for(auto& c:value) if(!(fields>>c) || c!=expected) throw std::runtime_error("Only normalized LUT domain supported");
        } else {
            if(!lut.size) throw std::runtime_error("LUT size must precede data");
            data=true;std::array<float,3> value{};std::istringstream first(token);char tail;
            if(!(first>>value[0]) || first>>tail || !(fields>>value[1]>>value[2])) throw std::runtime_error("Invalid LUT RGB row");
            for(float c:value) if(!std::isfinite(c) || c<0 || c>1) throw std::runtime_error("LUT RGB must be finite [0,1]");
            if(lut.rgb.size()>=size_t(lut.size)*lut.size*lut.size) throw std::runtime_error("Excess LUT rows");lut.rgb.push_back(value);
        }
        std::string extra;if(fields>>extra) throw std::runtime_error("Unexpected LUT field");
    }
    if(!stream.eof() || !lut.size || lut.rgb.size()!=size_t(lut.size)*lut.size*lut.size) throw std::runtime_error("Incomplete LUT");
    return lut;
}
}
#endif
