#include "Engine/Source/Base/Coordinate.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void CheckSize(float width, float height) {
    const float aspect = CoordinateWindowAspect(width, height);
    Check(std::fabs(aspect - width / height) < 0.00001f, "Window aspect lost live dimensions");
    CoordinateSetAspectRatio(aspect);
    const float scale = CoordinateWindowUiScale(width, height);
    Check(scale > 0 && scale <= 1, "Automatic UI scale is invalid");
    const float nominalLayoutWidth = 1024.0f * (height / 768.0f) * scale;
    Check(nominalLayoutWidth <= width + 0.01f, "Classic UI layout cannot fit the tile width");
    const float side = 0.05f * scale;
    const float pixelsX = DDCToNDCWidth(side) * width;
    const float pixelsY = DDCToNDCHeight(side) * height;
    Check(std::fabs(pixelsX - pixelsY) < 0.001f * pixelsY, "Square UI elements stretch after resize");
    Check(std::fabs(NDCToDDCWidth(DDCToNDCWidth(side)) - side) < 0.00001f, "Horizontal coordinates fail round-trip");
    Check(std::fabs(NDCToDDCHeight(DDCToNDCHeight(side)) - side) < 0.00001f, "Vertical coordinates fail round-trip");
}
}
int main() {
    try {
        CoordinateSetAspectRatio(4.0f / 3.0f);
        const float persistentUiWidth = NDCToDDCWidth(120.0f / (CoordinateGetAspectCompensation() * 1024.0f));
        // Repeated portrait/ultrawide transitions model a live tiling compositor.
        for (int repeat = 0; repeat < 3; ++repeat) {
            CheckSize(1024, 768);
            CheckSize(948, 1236);
            CheckSize(629, 1236);
            CheckSize(1920, 1080);
            CheckSize(5120, 1440);
            CheckSize(7680, 4320);
            CheckSize(640, 480);
            for (float aspect : {0.5f, 4.0f / 3.0f, 16.0f / 9.0f, 32.0f / 9.0f}) {
                CoordinateSetAspectRatio(aspect);
                const float newlyCreatedUiWidth = NDCToDDCWidth(120.0f / (CoordinateGetAspectCompensation() * 1024.0f));
                Check(std::fabs(persistentUiWidth - newlyCreatedUiWidth) < 0.000001f,
                    "Resize changed stored logical UI units and would require recreating frames");
            }
        }
        Check(std::fabs(CoordinateWindowUiScale(1920, 1080) - 768.0f / 1080.0f) < 0.00001f,
            "Automatic scale ignored the viewport height");
        Check(CoordinateWindowUiScale(629, 1236) < 0.64f,
            "The legacy minimum scale still clips narrow windows");
        Check(CoordinateWindowUiScale(0, 0) == 1.0f, "Minimised auto-scale is invalid");
        const float fallback = 4.0f / 3.0f;
        Check(CoordinateWindowAspect(0, 0) == fallback, "Minimised window produced an invalid aspect");
        Check(CoordinateWindowAspect(100, 0) == fallback, "Zero height was divided by");
        Check(CoordinateWindowAspect(-1, 100) == fallback, "Negative width was accepted");
        Check(CoordinateWindowAspect(std::numeric_limits<float>::infinity(), 100) == fallback, "Infinite width was accepted");
        Check(CoordinateWindowAspect(100, std::numeric_limits<float>::quiet_NaN()) == fallback, "NaN height was accepted");
        std::cout << "Live-window aspect, uniform UI geometry and resize round-trips passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
