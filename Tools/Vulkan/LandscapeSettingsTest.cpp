#include "WoW/Source/WorldClient/LandscapeSettings.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        using namespace LandscapeSettings;
        for (const char* text : {"0","4000","8000","10000"," 7500 ","4500.5"})
            Check(Parse(text).has_value(),"Valid landscape setting rejected");
        for (const char* text : {""," ","bad","nan","inf","-inf","1e40","-1","3999","10001","4000junk"})
            Check(!Parse(text).has_value(),"Invalid landscape setting accepted");
        Check(!Parse(nullptr),"Null landscape setting accepted");
        Check(Horizon(350,4,0)==1400 && FogScale(350,0)==1,"Classic horizon/fog changed");
        Check(Horizon(792,4,8000)==8000,"Landscape setting did not change actual horizon");
        Check(std::fabs(500*FogScale(500,8000)-8000)<.001,"Fog still ends before the selected horizon");
        Check(std::fabs(777*FogScale(777,10000)-10000)<.001,"10000-unit fog endpoint retained the Classic cap");
        Check(std::fabs(200*FogScale(500,8000)/8000-.4f)<.00001,"Authored fog shape changed");
        Check(FogScale(0,8000)==1,"Uninitialised scene divided by zero");
        for (int i=0;i<=SliderMaximum;++i) {
            const float value=FromSlider(i);
            Check(Parse(std::to_string(value).c_str()).has_value(),"Slider emitted invalid CVar");
            Check(ToSlider(value)==i,"Landscape slider roundtrip failed");
        }
        std::cout<<"Landscape setting: Classic parity, parsing/bounds, linked fog and slider range passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
