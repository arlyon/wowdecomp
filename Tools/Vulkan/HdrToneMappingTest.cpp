#include "Engine/Source/Gx/CGxDeviceVulkan/HdrToneMapping.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void Near(float a, float b) { Check(std::abs(a - b) < 1e-6f, "curve mismatch"); }
} // namespace
int main() {
    try {
        using namespace GxVulkan;
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        for (float ev : {-9.f, 9.f, nan, inf, -inf}) {
            Check(!HdrExposureValid(ev), "invalid EV admitted");

        }
        for (float ev : {-8.f, -2.f, 0.f, 2.f, 8.f}) {
            Check(HdrExposureValid(ev), "valid EV rejected");
            float previous = -1;
            for (float value : {.01f, .5f, 4.f, 20.f}) {
                float out = HdrToneMapLinear(value, ev);
                Check(std::isfinite(out) && out > previous && out < 1, "ordering/finite shoulder");
                Check(out == HdrToneMapLinear(value, ev), "manual EV nondeterministic");
                previous = out;
            }
            Check(std::isfinite(HdrToneMapLinear(std::numeric_limits<float>::max(), ev)), "finite input overflow");
        }
        Near(HdrToneMapLinear(.01f, 0), .01f);
        Near(HdrToneMapLinear(.5f, 0), .5f);
        Near(HdrToneMapLinear(4, 0), .9375f);
        Near(HdrToneMapLinear(20, 0), .9875f);
        Near(HdrToneMapLinear(.01f, -2), .0025f);
        Near(HdrToneMapLinear(.01f, 2), .04f);
        Near(HdrToneMapLinear(.001f, 0), .001f); // no night/cave normalization
        for (float invalid : {-1.f, nan, inf, -inf})
            Near(HdrToneMapLinear(invalid, 0), 0);
        Near(HdrEncodeSdr(0), 0);
        Near(HdrEncodeSdr(1), 1);
        Near(HdrEncodeSdr(.001f), .01292f);
        float left = HdrToneMapLinear(.5f - 1e-4f, 0), right = HdrToneMapLinear(.5f + 1e-4f, 0);
        Check(std::abs((right - left) / 2e-4f - 1) < .001f, "knee must be C1 continuous");
        std::cout << "PASS: default-off admission, EV bounds/NaN, deterministic ordering, shoulder, dark identity, "
                     "explicit SDR encoding\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
