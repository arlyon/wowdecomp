#include "Engine/Source/Gx/CGxDeviceVulkan/TemporalBackend.h"
#include <stdexcept>
#include <iostream>
void Check(bool value) { if (!value) throw std::runtime_error("Temporal snapshot contract failed"); }
int main() {
    using Draws = GxVulkan::TemporalDraws;
    Draws draws;
    int owner, other, buffer;
    const Draws::Key key{reinterpret_cast<uint64_t>(&owner),1,2,3};
    const Draws::Key instance{reinterpret_cast<uint64_t>(&other),1,2,3};
    Draws::Pose pose{{1,2,3,4,5,6,7,8},{&buffer},.2f,.8f,true};
    pose.constants.resize(1024); pose.constants[1023]=123.5f; // Entire register/bone palette.
    Check(!draws.Previous(key)); draws.Record(key,pose);
    Check(!draws.Previous(key)); draws.Commit();
    Check(draws.Previous(key) && draws.Previous(key)->constants == pose.constants);
    Check(!draws.Previous(instance));
    draws.Record(key,pose); Check(!draws.Ambiguous());
    draws.Record(key,pose); Check(draws.Ambiguous());
    draws.Commit(); Check(!draws.Ambiguous());
    Check(!draws.Previous(key));
    draws.Record(key,pose); draws.Commit(); draws.ForgetBuffer(&buffer); Check(!draws.Previous(key));
    draws.Record(key,pose); draws.ForgetBuffer(&buffer); draws.Commit(); Check(!draws.Previous(key));
    draws.Record(key,pose); draws.Commit(); draws.ForgetOwner(&owner); Check(!draws.Previous(key));
    draws.Record(key,pose); draws.Commit(); draws.Commit(); Check(!draws.Previous(key));
    draws.Record(key,pose); draws.Commit(); draws.Reset(); Check(!draws.Previous(key));
    bool threw = false;
    try { GxVulkan::TemporalViewportSource("unexpected wrapper"); } catch (const std::runtime_error&) { threw = true; }
    Check(threw);
    std::cout << "Temporal draws: explicit instance, submitted generations, full palette, ambiguity, mutation, destruction, absent trim, reset passed\n";
}
