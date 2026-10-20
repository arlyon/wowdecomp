#include "Engine/Source/Gx/CGxDeviceVulkan/RayQueryCapabilities.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace GxVulkan;
void Check(bool value) {
    if (!value)
        throw std::runtime_error("ray-query capability policy assertion");
}
int main() {
    try {
        RayQueryCapabilities capabilities;
        Check(!capabilities.Available() && capabilities.Missing() == RayQueryRequirement::Core12);
        capabilities.supported.fill(true);
        Check(capabilities.Available() && std::strcmp(capabilities.Reason(), "available") == 0);
        for (size_t i = 0; i < RayQueryRequirementCount; ++i) {
            const auto requirement = static_cast<RayQueryRequirement>(i);
            capabilities.Set(requirement, false);
            Check(!capabilities.Available() && capabilities.Missing() == requirement);
            Check(capabilities.Reason() && *capabilities.Reason() &&
                  std::strcmp(capabilities.Reason(), "available") != 0);
            capabilities.Set(requirement, true);
        }
        bool rejected = false;
        try {
            capabilities.Set(RayQueryRequirement::Count, true);
        } catch (const std::out_of_range &) {
            rejected = true;
        }
        Check(rejected);
        std::cout << "PASS all missing API/queue/extension/feature requirements reject capability, defaults fail "
                     "closed; no scene readiness implied\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
