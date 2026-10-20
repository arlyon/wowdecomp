#include "Engine/Source/Gx/CGxDeviceVulkan/TemporalHistory.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected,"Invalid temporal state transition was accepted");
}
}
int main() {
    try {
        using namespace GxVulkan;
        TemporalHistory history;
        TemporalFrameDescription frame{1,7,1920,1280,{0,0,1920,1280},true,false};
        Reject([&] { history.Commit(); });
        const auto first = history.Begin(frame);
        Check(first.active && !first.historyValid && first.sample==0,"First frame reused stale history");
        Check(std::fabs(first.jitterClip[0]*960-first.jitterPixels[0])<.000001f &&
            std::fabs(first.jitterClip[1]*-640-first.jitterPixels[1])<.000001f,"Jitter used wrong viewport units or Y sign");
        Reject([&] { history.Begin(frame); }); history.Commit();
        for (unsigned i = 1; i < 24; ++i) {
            ++frame.frame; const auto state = history.Begin(frame);
            Check(state.historyValid && state.sample==i%8,"Consecutive history or jitter cycle lost"); history.Commit();
        }
        float sumX=0,sumY=0;
        for (unsigned i=0;i<8;++i) {
            const auto jitter=TemporalJitterPixels(i);
            Check(std::fabs(jitter[0])<=.5f && std::fabs(jitter[1])<=.5f,"Jitter escaped a pixel footprint");
            Check(jitter==TemporalJitterPixels(i+8),"Jitter cycle is not deterministic");
            sumX+=jitter[0];sumY+=jitter[1];
        }
        Check(std::fabs(sumX)<.000001f && std::fabs(sumY)<.000001f,"Finite jitter cycle has a directional bias");
        const auto reset = [&] {
            const auto state=history.Begin(frame);
            Check(state.active && !state.historyValid && state.sample==0,"Invalidated history was reused");history.Commit();
        };
        frame.frame+=2;reset(); // UI-only/presentation gap.
        ++frame.frame;frame.cameraCut=true;reset();frame.cameraCut=false;
        ++frame.frame;++frame.scene;reset();
        ++frame.frame;frame.width=712;frame.height=1236;frame.viewport={0,0,712,1236};reset();
        ++frame.frame;frame.viewport={10,20,600,1100};reset();
        ++frame.frame;frame.enabled=false;
        Check(!history.Begin(frame).active,"Disabled temporal mode still active");
        Reject([&] { history.Commit(); });
        ++frame.frame;frame.enabled=true;reset();
        ++frame.frame;frame.width=0;
        Check(!history.Begin(frame).active,"Minimised output attempted temporal work");
        ++frame.frame;frame.width=712;reset();
        ++frame.frame;history.Begin(frame);history.Cancel();++frame.frame;reset();
        frame.frame=std::numeric_limits<uint64_t>::max();reset();frame.frame=1;reset();
        ++frame.frame;frame.viewport={700,0,100,1};Reject([&] { history.Begin(frame); });
        frame.viewport={0,0,std::numeric_limits<uint32_t>::max(),1};Reject([&] { history.Begin(frame); });
        frame.viewport={0,0,600,1100};frame.frame=0;Reject([&] { history.Begin(frame); });
        std::cout<<"Temporal history: jitter, cuts, scene/viewport changes, gaps, disable/minimise, cancellation and bounds passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
