#include "WoW/Source/WorldClient/CloudNoise.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    try {
        // Preserve every possible Windows CRT sample, not just its endpoints.
        for (unsigned value = 0; value <= 32767; ++value) {
            const float legacy = 1.0f - (value * 0.000030518509f + value * 0.000030518509f);
            Check(DayNight::CloudNoiseSample(value,32767) == legacy, "Windows cloud noise changed");
        }
        for (unsigned maximum : {32767u,2147483647u}) {
            Check(DayNight::CloudNoiseSample(0,maximum) == 1, "Minimum RNG endpoint is incorrect");
            Check(DayNight::CloudNoiseSample(maximum,maximum) == -1, "Maximum RNG endpoint is incorrect");
            for (unsigned i = 0; i <= 65536; ++i) {
                const unsigned value = static_cast<unsigned>((static_cast<unsigned long long>(maximum)*i)/65536);
                const float noise = DayNight::CloudNoiseSample(value,maximum);
                Check(std::isfinite(noise) && noise >= -1 && noise <= 1, "Cloud noise escaped its interpolation range");
            }
        }
        for (unsigned i = 0; i < 256; ++i) {
            const float noise = DayNight::CloudNoiseSample(std::rand(),RAND_MAX);
            Check(noise >= -1 && noise <= 1, "Actual host CRT produces invalid cloud noise");
        }
        // Five amplitudes sum to 1.9375. Valid noise keeps the generated
        // pre-sharpness density in the byte range instead of wrapping randomly.
        const float sum = 1 + .5f + .25f + .125f + .0625f;
        Check(128 - sum*64 >= 0 && 128 + sum*64 <= 255, "Cloud density can wrap before sharpness lookup");
        std::cout << "Cloud noise: exact Windows equivalence and bounded Linux/host samples passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
