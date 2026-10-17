#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/Frame/CLayoutFrame.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Near(float actual, float expected, const char* message) {
    Check(std::fabs(actual - expected) < 0.00001f, message);
}
class ScreenRoot : public CLayoutFrame {
public:
    unsigned changes = 0;
    void OnFrameSizeChanged(const CRect& previous) override {
        ++changes;
        CLayoutFrame::OnFrameSizeChanged(previous);
    }
};
}
int main() {
    try {
        CoordinateSetAspectRatio(1.5f);
        ScreenRoot root;
        root.RefreshScreenRect();
        CLayoutFrame fill, corner;
        fill.SetAllPoints(&root, 1);
        corner.SetSize(.04f, .04f);
        corner.SetPoint(FRAMEPOINT_TOPRIGHT, &fill, FRAMEPOINT_TOPRIGHT, -.01f, -.01f, 1);
        // Reuse the same root and anchors. Recreating them would hide the bug.
        const float dimensions[][2] = {{1920,1280},{1428,1860},{712,1236},{2560,1080},{1920,1280}};
        for (const auto& size : dimensions) {
            const float width = size[0], height = size[1];
            CoordinateSetAspectRatio(CoordinateWindowAspect(width,height));
            root.RefreshScreenRect();
            CLayoutFrame::ResizePending();
            CRect rootRect{}, fillRect{}, cornerRect{};
            Check(root.GetRect(&rootRect) && fill.GetRect(&fillRect) && corner.GetRect(&cornerRect), "Resize invalidated layout rectangles");
            Near(rootRect.maxX, NDCToDDCWidth(1), "Screen projection retained its old aspect");
            Near(rootRect.maxY, NDCToDDCHeight(1), "Screen projection retained its old height");
            Near(fillRect.maxX, rootRect.maxX, "Root resize did not invalidate dependent anchors");
            Near(cornerRect.maxX, rootRect.maxX - .01f, "Right-anchored widget retained its old position");
            const float geometryX = cornerRect.minX / rootRect.maxX;
            const float textX = DDCToNDCWidth(cornerRect.minX);
            Near(geometryX, textX, "Text and geometry use different screen projections");
            const float left = DDCToNDCWidth(cornerRect.minX), right = DDCToNDCWidth(cornerRect.maxX);
            const float bottom = DDCToNDCHeight(cornerRect.minY), top = DDCToNDCHeight(cornerRect.maxY);
            Check(left >= 0 && right <= 1 && bottom >= 0 && top <= 1, "Widget viewport escaped the presentation window");
            Near((right-left)*width/height, top-bottom, "Square UI widget became stretched");
            const auto changes = root.changes;
            root.RefreshScreenRect();
            Check(changes == root.changes, "Unchanged dimensions caused a relayout storm");
        }
        std::cout << "Persistent screen root, dependent anchors, text/geometry alignment and viewport resize passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
