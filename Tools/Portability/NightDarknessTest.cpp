#include "WoW/Source/WorldClient/NightDarkness.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void Check(bool value) { if (!value) throw std::runtime_error("Night ambient contract failed"); }
float Scale(float hour,float amount=80,bool protect=false) { return NightDarkness::AmbientScale(hour/24.f,amount,protect); }
}
int main() {
    try {
        for (const char* text : {"0","50","100"," 80 ","12.5"}) Check(NightDarkness::Parse(text).has_value());
        for (const char* text : {"","-1","101","nan","inf","1e40","80junk"}) Check(!NightDarkness::Parse(text));
        Check(!NightDarkness::Parse(nullptr));
        Check(Scale(12)==1 && Scale(6)==1 && Scale(18)==1);
        Check(std::abs(Scale(0)-.2f)<.00001 && Scale(0)==Scale(24) && Scale(-24)==Scale(0));
        Check(Scale(20)>Scale(22) && Scale(20)<Scale(18));
        Check(Scale(5)>Scale(4) && Scale(5)<Scale(6));
        Check(std::abs(Scale(24-.001f)-Scale(.001f))<.0001);
        Check(Scale(0,80,true)==1 && Scale(0,0)==1);
        Check(NightDarkness::AmbientScale(std::numeric_limits<float>::quiet_NaN(),80)==1);
        Check(NightDarkness::AmbientScale(0,std::numeric_limits<float>::infinity())==1);
        for (unsigned i=0;i<256;++i) {
            const CImVector original(i,255-i,i/2,173);
            const auto classic=NightDarkness::Ambient(original,Scale(0,0));
            Check(classic.r==original.r && classic.g==original.g && classic.b==original.b && classic.a==173);
            const auto darker=NightDarkness::Ambient(original,Scale(0,80));
            Check(darker.r<=original.r && darker.g<=original.g && darker.b<=original.b && darker.a==173);
        }
        std::cout<<"Night ambient: Classic parity, bounds, twilight continuity, midnight wrap, protected lighting and alpha preservation passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
