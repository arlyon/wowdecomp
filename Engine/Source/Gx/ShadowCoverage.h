#ifndef GX_SHADOW_COVERAGE_H
#define GX_SHADOW_COVERAGE_H
#include <cstdint>
#include <stdexcept>
#include <variant>

namespace GxShadow {
// Authored exclusions and missing implementation are different states. Neither
// may be inferred from a shader fingerprint or from absent texture readiness.
enum class ExclusionReason { BatchExcluded, MaterialNoCast, BlendNotForced, AnimatedAlpha, NoDirectionalLight, NotWorldSurface };
enum class MissingSupport { MaterialImplementation, MaintainedReceiver, CoverageInterface };
struct UnsupportedCoverage { MissingSupport reason=MissingSupport::CoverageInterface; };
struct Opaque {};
struct Cutout {};
struct ForcedCutout {};
struct NonCaster { ExclusionReason reason; };
template<class... Extra> using CoverageCases=std::variant<UnsupportedCoverage,Opaque,Cutout,ForcedCutout,NonCaster,Extra...>;
using Coverage=CoverageCases<>;
enum class CoverageMode : uint32_t { Opaque=0, Cutout=1, ForcedCutout=2, NonCaster=3 };
struct CoverageInfo { CoverageMode mode; bool casts, needsCoverageTexture; };
struct CoverageTraits {
    CoverageInfo operator()(UnsupportedCoverage)const{throw std::logic_error("Required shadow coverage is unsupported");}
    constexpr CoverageInfo operator()(Opaque)const{return {CoverageMode::Opaque,true,false};}
    constexpr CoverageInfo operator()(Cutout)const{return {CoverageMode::Cutout,true,true};}
    constexpr CoverageInfo operator()(ForcedCutout)const{return {CoverageMode::ForcedCutout,true,true};}
    constexpr CoverageInfo operator()(NonCaster)const{return {CoverageMode::NonCaster,false,false};}
};
inline CoverageInfo Describe(const Coverage& value){return std::visit(CoverageTraits{},value);}

struct CastAndReceive {};
struct CastOnly {};
struct ReceiveOnly {};
struct IntentionallyExcluded { ExclusionReason reason; };
struct UnsupportedRequired { MissingSupport reason=MissingSupport::MaterialImplementation; };
template<class... Extra> using ParticipationCases=std::variant<UnsupportedRequired,CastAndReceive,CastOnly,ReceiveOnly,IntentionallyExcluded,Extra...>;
using Participation=ParticipationCases<>;
struct ParticipationInfo { bool casts,receives,requiredSupported; };
struct ParticipationTraits {
    constexpr ParticipationInfo operator()(CastAndReceive)const{return {true,true,true};}
    constexpr ParticipationInfo operator()(CastOnly)const{return {true,false,true};}
    constexpr ParticipationInfo operator()(ReceiveOnly)const{return {false,true,true};}
    constexpr ParticipationInfo operator()(IntentionallyExcluded)const{return {false,false,true};}
    constexpr ParticipationInfo operator()(UnsupportedRequired)const{return {false,false,false};}
};
inline ParticipationInfo Describe(const Participation& value){return std::visit(ParticipationTraits{},value);}
// A required unsupported surface cannot quietly become an intentional exclusion
// when packing the GPU scene. Readiness/accounting must reject it first.
inline uint32_t Encode(const Participation& value){
    const auto info=Describe(value);
    if(!info.requiredSupported)throw std::logic_error("Required shadow participation is unsupported");
    return uint32_t(info.casts)|(uint32_t(info.receives)<<1);
}
}
#endif
