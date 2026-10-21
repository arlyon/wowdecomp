#include "Engine/Source/Gx/ShadowCoverage.h"
#include "Engine/Source/Model2/ShadowSelection.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace GxShadow;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f){bool failed=false;try{f();}catch(const std::logic_error&){failed=true;}Check(failed,"unsupported required state was silently encoded");}
int main(){try {
    Check(Describe(Coverage{Opaque{}}).casts && !Describe(Coverage{Opaque{}}).needsCoverageTexture,"opaque casting depends on a texture");
    Check(Describe(Coverage{Cutout{}}).needsCoverageTexture && Describe(Coverage{ForcedCutout{}}).needsCoverageTexture,"coverage material lost its texture requirement");
    Check(Encode(Participation{CastAndReceive{}})==3 && Encode(Participation{CastOnly{}})==1 && Encode(Participation{ReceiveOnly{}})==2,"participation contract changed");
    Check(Encode(Participation{IntentionallyExcluded{ExclusionReason::NotWorldSurface}})==0,"intentional exclusion became required");
    Reject([]{Encode(Participation{});});Reject([]{Describe(Coverage{});});
    Reject([]{Encode(Participation{UnsupportedRequired{MissingSupport::MaintainedReceiver}});});
    Check(!M2Shadow::BatchExclusion(0,0,0) && M2Shadow::BatchExclusion(0x8000,0,0) && M2Shadow::BatchExclusion(0,4,0) && M2Shadow::BatchExclusion(0,0,1),"authored M2 batch selection changed");
    const float boundary=.55000001f;
    const float opacity[]={0,std::nextafter(boundary,0.f),boundary,std::nextafter(boundary,1.f),1,std::numeric_limits<float>::quiet_NaN()};
    unsigned cases=0;
    for(uint16_t flags=0;flags<256;++flags)for(uint16_t blend=0;blend<7;++blend)for(float alpha:opacity) {
        // Independent transcription of original selection, not the new helper.
        const bool original=!(flags&0x40) && ((flags&0x80) || blend<=1) && alpha>=boundary;
        const auto coverage=M2Shadow::MaterialCoverage(flags,blend,alpha);const auto info=Describe(coverage);
        Check(info.casts==original,"M2 authored/fade shadow selection diverged");
        if(original)Check(info.needsCoverageTexture==bool(blend),"M2 opaque/textured routing changed");
        if(original && blend>1)Check(info.mode==CoverageMode::ForcedCutout,"forced blended shadow was dropped or relabelled");
        ++cases;
    }
    std::cout<<"PASS closed shadow participation/coverage and "<<cases<<" original M2 authored/fade combinations; default required states fail closed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL shadow contracts: "<<e.what()<<'\n';return 1;}}
