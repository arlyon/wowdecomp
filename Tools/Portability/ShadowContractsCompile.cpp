// Positive control and exactly-one-missing-handler negative compilations.
#include "Engine/Source/Gx/ShadowCoverage.h"
using namespace GxShadow;
struct NewCoverage {};
struct NewParticipation {};
struct CompleteCoverage : CoverageTraits {
    using CoverageTraits::operator();
    CoverageInfo operator()(NewCoverage)const{return {CoverageMode::Cutout,true,true};}
};
struct CompleteParticipation : ParticipationTraits {
    using ParticipationTraits::operator();
    ParticipationInfo operator()(NewParticipation)const{return {true,false,true};}
};
int main(){
    CoverageCases<NewCoverage> coverage{NewCoverage{}};
    ParticipationCases<NewParticipation> participation{NewParticipation{}};
#if defined(TEST_UNHANDLED_COVERAGE)
    (void)std::visit(CoverageTraits{},coverage);
#else
    (void)std::visit(CompleteCoverage{},coverage);
#endif
#if defined(TEST_UNHANDLED_PARTICIPATION)
    (void)std::visit(ParticipationTraits{},participation);
#else
    (void)std::visit(CompleteParticipation{},participation);
#endif
    (void)Describe(Coverage{Opaque{}});
    (void)Encode(Participation{CastAndReceive{}});
}
