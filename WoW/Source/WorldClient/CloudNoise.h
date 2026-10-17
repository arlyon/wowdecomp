#ifndef WOW_SOURCE_WORLDCLIENT_CLOUDNOISE_H
#define WOW_SOURCE_WORLDCLIENT_CLOUDNOISE_H

namespace DayNight {
// Inputs follow the CRT rand() contract: maximum > 0, value in [0, maximum].
// The original reciprocal of 32767 assumes the Windows CRT's 15-bit range.
inline float CloudNoiseSample(unsigned value, unsigned maximum) {
    const float scale = 1.0f / static_cast<float>(maximum);
    const float normalized = static_cast<float>(value) * scale;
    return 1.0f - (normalized + normalized);
}
}
#endif
