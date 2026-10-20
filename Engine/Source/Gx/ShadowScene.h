#ifndef GX_SHADOW_SCENE_H
#define GX_SHADOW_SCENE_H
#include "ShadowCoverage.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <map>
#include <vector>
class CGxTex;
namespace GxShadow {
enum class FrameResult { Disabled, Ready, Unavailable, PendingGeometry, PendingTexture, StalePose, InvalidGeometry, UnsupportedCoverage, AllocationFailed };
// Explicit interface-v2 wire types, independent of CPU lighting/object layouts.
struct alignas(16) GpuTriangle { float uv01[4],uv2OpacityThreshold[4];uint32_t flags[4]; };
struct alignas(16) GpuDirectionalFrame { float values[7][4]; };
static_assert(sizeof(GpuTriangle)==48&&offsetof(GpuTriangle,flags)==32,"Shared triangle coverage ABI");
static_assert(sizeof(GpuDirectionalFrame)==112,"Directional frame ABI");
struct Position { float xyz[3]; };
struct CoverageVertex { float uv[4][2]{}; };
struct Material {
    CoverageMode mode=CoverageMode::Opaque;
    float opacity=1, threshold=0;
    bool twoSided=true;
    uint32_t textureCount=0;
    std::array<CGxTex*,4> textures{};
    std::array<bool,4> wrapU{},wrapV{};
    std::array<std::shared_ptr<const void>,4> textureOwners;
};
enum class GeometryKind { Terrain, Model, Building };
struct Mesh {
    GeometryKind kind=GeometryKind::Terrain;
    std::vector<Position> positions;
    std::vector<CoverageVertex> coverage;
    std::vector<uint32_t> indices;
    Material material;
};
struct Status {
    FrameResult result=FrameResult::Disabled;
    uint64_t terrain=0,models=0,buildings=0,triangles=0,unsupported=0;
    bool diagnosticRendered=false;
    uint64_t terrainReceivers=0;
    bool terrainRendered=false;
    std::string detail;
};
struct Frame {
    FrameResult result=FrameResult::Disabled;
    std::vector<Mesh> meshes;
    std::array<float,16> inverseViewProjection{},viewInverse{};
    std::array<float,2> viewportDepthRange{0.f,1.f};
    std::array<float,3> camera{},lightDirection{};
    float regionRadius=64,rayDistance=128;
    uint64_t terrain=0,models=0,buildings=0,triangles=0;
    uint64_t nonCasters=0,unready=0,unsupported=0;
    double collectionMilliseconds=0;
    std::map<std::string,uint64_t> modelIssues;
    std::string detail;
};
}
#endif
