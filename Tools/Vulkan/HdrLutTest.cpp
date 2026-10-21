#include "Engine/Source/Gx/CGxDeviceVulkan/HdrLut.h"
#include <iostream>
namespace {
void Check(bool b,const char* s) {if(!b) throw std::runtime_error(s);}
std::string Cube(bool look) {
    std::ostringstream text;text<<"TITLE \"source-owned fixture\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for(unsigned b=0;b<2;++b) for(unsigned g=0;g<2;++g) for(unsigned r=0;r<2;++r) text<<(look?r*.5f:r)<<' '<<g<<' '<<b<<'\n';return text.str();
}
void Reject(const std::string& text) {bool rejected=false;try {std::istringstream s(text);GxVulkan::ReadHdrCube(s);}catch(const std::exception&){rejected=true;}Check(rejected,"invalid cube admitted");}
}
int main() {
    try {
        for(bool look:{false,true}) {std::istringstream input(Cube(look));auto lut=GxVulkan::ReadHdrCube(input);Check(lut.size==2 && lut.rgb.size()==8,"cube extent");Check(lut.rgb[0]==std::array<float,3>{0,0,0},"black changed");Check(lut.rgb[1][0]==(look?.5f:1.f) && lut.rgb[2][1]==1 && lut.rgb[4][2]==1,"red-fastest order");}
        for(const char* bad:{"","LUT_3D_SIZE 1\n","LUT_3D_SIZE 34\n","LUT_1D_SIZE 2\n","LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n","LUT_3D_SIZE 2\nNaN 0 0\n","LUT_3D_SIZE 2\n0 0 inf\n","LUT_3D_SIZE 2\n-1 0 0\n"}) Reject(bad);
        Reject(Cube(false)+"0 0 0\n");Reject("LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n");Reject(std::string(5000,'x'));
        std::cout<<"PASS: strict bounded normalized 3D cube, red-fastest identity/look, black, invalid input\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
