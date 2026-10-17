#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/FFX/FFX.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
unsigned PowerOfTwo(unsigned x) { unsigned n = 1; while (n < x) n *= 2; return n; }
struct Point { double x, y, mass; };
Point Centroid(CGxDeviceVulkan& device, int width, int height, std::vector<double>* capture = nullptr) {
    CiRect rect; rect.minX = rect.minY = 0; rect.maxX = width; rect.maxY = height;
    TSGrowableArray<CImVector> pixels;
    device.DeviceReadPixels(rect, pixels);
    Check(pixels.Count() == unsigned(width * height), "Incomplete production readback");
    Point p{};
    if (capture) capture->resize(size_t(width) * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto c = pixels[y * width + x];
        const double weight = (double(c.r) + c.g + c.b) / (3 * 255.0);
        if (capture) (*capture)[y * width + x] = weight;
        p.x += (x + .5) * weight; p.y += (y + .5) * weight; p.mass += weight;
    }
    Check(p.mass > 1, "FFX marker disappeared");
    p.x /= p.mass; p.y /= p.mass; return p;
}
// Model the existing FFX +half-texel UV convention and UNORM bilinear
// filtering, not an ideal resize. Its sampling-centre bias is separate from
// a render-target-origin error and must not be hidden by a loose tolerance.
std::vector<double> Resample(const std::vector<double>& source, int sw, int sh, int dw, int dh) {
    std::vector<double> out(size_t(dw) * dh);
    const auto texel = [&](int x, int y) { return x < 0 || y < 0 || x >= sw || y >= sh ? 0.0 : source[y * sw + x]; };
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
        const double sx = (x + .5) * sw / dw, sy = (y + .5) * sh / dh;
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        const double fx = sx - ix, fy = sy - iy;
        const double value = (1-fy)*((1-fx)*texel(ix,iy)+fx*texel(ix+1,iy)) +
                            fy*((1-fx)*texel(ix,iy+1)+fx*texel(ix+1,iy+1));
        out[y * dw + x] = std::round(value * 255) / 255;
    }
    return out;
}
Point Expected(const std::vector<double>& image, int width, int height) {
    Point p{};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const double v = image[y * width + x];
        p.x += (x + .5) * v; p.y += (y + .5) * v; p.mass += v;
    }
    Check(p.mass > 1, "Invalid CPU marker oracle"); p.x /= p.mass; p.y /= p.mass; return p;
}
void Ortho(CGxDeviceVulkan& device, int width, int height) {
    C44Matrix matrix;
    GxuXformCreateOrtho(0, float(width), 0, float(height), -1, 1, matrix);
    device.XformSetProjection(matrix);
}
void Quad(CGxDeviceVulkan& device, const C3Vector* positions, const C2Vector* uv, CGxTex* texture) {
    const CImVector white(255, 255, 255, 255);
    const uint16_t indices[] = {0, 3, 1, 2};
    device.RsSet(GxRs_Texture0, texture);
    GxPrimVertexPtr(4, positions, sizeof(C3Vector), nullptr, 0, &white, 0,
                    uv, uv ? sizeof(C2Vector) : 0, nullptr, 0);
    GxPrimIndexPtr(4, indices);
    CGxBatch batch{}; batch.m_primType = GxPrim_TriangleStrip; batch.m_count = 4; batch.m_maxIndex = 3;
    device.Draw(&batch, 1);
}
struct Image { CGxTex* texture = nullptr; C2iVector active{}, backing{}; };
Image Make(CGxDeviceVulkan& device, int width, int height, bool depth = false) {
    Image image; image.active = {width, height};
    image.backing = {int(PowerOfTwo(width)), int(PowerOfTwo(height))};
    const auto format = depth ? GxTex_D24X8 : GxTex_Argb8888;
    Check(device.TexCreate(GxTex_2d, image.backing.x, image.backing.y, 0, format, format,
          CGxTexFlags(GxTex_Linear, 0, 0, 0, 0, 1), nullptr, nullptr, "FFX alignment fixture", image.texture) != 0,
          "Production render texture creation failed");
    return image;
}
void Target(CGxDeviceVulkan& device, const Image* image, const Image* depth) {
    device.DeviceSetRenderTarget(GxBuffers_Depth, depth ? depth->texture : nullptr, 0);
    device.DeviceSetRenderTarget(GxBuffers_Color, image ? image->texture : nullptr, 0);
    device.SceneClear(3, CImVector(0, 0, 0, 255));
}
void Sample(CGxDeviceVulkan& device, const Image& source, C2iVector active, C2iVector backing) {
    C3Vector positions[4]; C2Vector uv[4];
    FFX::SetupGeo_Quad(active, source.active, source.backing, positions, uv, false);
    Ortho(device, active.x, active.y);
    device.XformSetViewport(0, float(active.x) / backing.x, 0, float(active.y) / backing.y, 0, 1);
    Quad(device, positions, uv, source.texture);
}
void Compare(Point reference, Point actual, const char* stage) {
    const double distance = std::hypot(reference.x - actual.x, reference.y - actual.y);
    std::cout << stage << " oracle=" << reference.x << ',' << reference.y
              << " actual=" << actual.x << ',' << actual.y << " oracle error=" << distance << '\n';
    Check(distance <= .25, "Production FFX copy/quad diverged from the pixel-centre oracle");
}
void Run(CGxDeviceVulkan& device, int requestedWidth, int requestedHeight) {
    SDL_SetWindowSize(static_cast<SDL_Window*>(device.DeviceWindow()), requestedWidth, requestedHeight);
    SDL_PumpEvents(); device.DeviceWM(GxWM_Size, 0, 0);
    Target(device, nullptr, nullptr); device.XformSetViewport(0, 1, 0, 1, 0, 1);
    const auto& rect = device.DeviceDefWindow();
    const int width = int(rect.maxX - rect.minX), height = int(rect.maxY - rect.minY);
    Check(width >= 32 && height >= 32, "Invalid fixture framebuffer size");
    std::cout << "driver=" << SDL_GetCurrentVideoDriver() << " requested=" << requestedWidth << 'x' << requestedHeight
              << " framebuffer=" << width << 'x' << height << '\n';
    Ortho(device, width, height);
    const float x = width * .37f, y = height * .61f;
    const C3Vector marker[] = {{x - 8, y - 8, 0}, {x - 8, y + 8, 0},
                              {x + 8, y + 8, 0}, {x + 8, y - 8, 0}};
    Quad(device, marker, nullptr, nullptr);
    std::vector<double> input;
    const Point reference = Centroid(device, width, height, &input);
    std::cout << "source marker=" << reference.x << ',' << reference.y << '\n';
    Image full = Make(device, width, height), half = Make(device, width / 2, height / 2),
          quarter = Make(device, width / 4, height / 4), depth = Make(device, width, height, true);
    // Initialise padding too: reading undefined texels must not decide the oracle.
    Target(device, &full, &depth);
    device.DeviceSetRenderTarget(GxBuffers_Depth, nullptr, 0);
    device.DeviceSetRenderTarget(GxBuffers_Color, nullptr, 0);
    const C2iVector zero{0, 0}, screen{width, height};
    Check(device.TexCopy(full.texture, zero, zero, screen, 0, 0) != 0, "Production backbuffer copy failed");
    // First distinguish copy/UV mapping from the downsample/render-target path.
    Target(device, nullptr, nullptr); Sample(device, full, screen, screen);
    Compare(Expected(Resample(input, width, height, width, height), width, height),
            Centroid(device, width, height), "backbuffer-copy/composite");
    Target(device, &half, &depth); Sample(device, full, half.active, half.backing);
    Target(device, &quarter, &depth); Sample(device, half, quarter.active, quarter.backing);
    Target(device, nullptr, nullptr); Sample(device, quarter, screen, screen);
    auto halfOracle = Resample(input, width, height, half.active.x, half.active.y);
    auto quarterOracle = Resample(halfOracle, half.active.x, half.active.y, quarter.active.x, quarter.active.y);
    auto finalOracle = Resample(quarterOracle, quarter.active.x, quarter.active.y, width, height);
    Compare(Expected(finalOracle, width, height), Centroid(device, width, height), "FFX half/quarter/composite");
    device.ScenePresent();
    for (auto* image : {&full, &half, &quarter, &depth}) device.TexDestroy(image->texture);
}
}
int main() {
    try {
        Check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        CGxDeviceVulkan device; g_theGxDevicePtr = &device;
        CGxFormat format; format.m_window = 1; format.m_maximize = 0; format.m_size = {320, 240};
        SDL_Window* window = SDL_CreateWindow("FFX alignment test (no game session)", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, format.m_size.x, format.m_size.y,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_UTILITY);
        Check(window != nullptr, SDL_GetError());
        Check(device.DeviceCreate(window, format) != 0, "Native device creation failed");
        device.m_cursorVisible = 0;
        for (auto state : {GxRs_Culling, GxRs_DepthTest, GxRs_DepthWrite, GxRs_Fog, GxRs_Lighting}) device.RsSet(state, 0);
        device.RsSet(GxRs_ColorWrite, 15); device.RsSet(GxRs_BlendingMode, 0);
        device.RsSet(GxRs_ColorOp0, 0); device.RsSet(GxRs_AlphaOp0, 3);
        device.RsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
        device.RsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
        C44Matrix identity; device.XformPushAndSet(GxXform_World, identity); device.XformSetView(identity);
        for (auto size : {C2iVector{320, 240}, C2iVector{480, 360}, C2iVector{640, 480}, C2iVector{960, 640}})
            Run(device, size.x, size.y);
        device.DeviceDestroy(); SDL_DestroyWindow(window); g_theGxDevicePtr = nullptr; SDL_Quit();
        std::cout << "PASS actual Native TexCopy + production FFX quad alignment across framebuffer/POT boundaries\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
