#ifndef GX_FAR_RESOURCES_H
#define GX_FAR_RESOURCES_H
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GxFar {
// Owner-thread leases, including final release. Only source/preparation staging
// is capped; resident resource metadata must not starve the next upload job.
// This FAR ownership seam has no ray texture snapshots or recapture state.
enum class FarCpuClass : uint8_t { Source, Prepared, ResourceMetadata };
constexpr size_t FarCpuBudget = 16 * 1024 * 1024;
constexpr size_t FarPreparedCpuBudget = 8 * 1024 * 1024;
constexpr size_t FarNearHeadroom = 13 * 1024 * 1024;
struct FarCpuCensus { size_t total = 0, source = 0, prepared = 0, resourceMetadata = 0; };
FarCpuCensus FarCpuBytes();
std::shared_ptr<const void> ReserveFarCpuBytes(size_t bytes, FarCpuClass classification);
#ifdef WOWDECOMP_FAR_TESTING
// Simulate external owner pressure without giving FAR ownership of near data.
void SetFarCpuPressureForTesting(size_t limit, size_t externalBytes = 0);
size_t FarCpuLiveBytesForTesting();
#endif
}

struct CGxBuf;
class CGxTex;

// Immutable upload spans, borrowed only for SceneCreateFarResources. Vertex
// layout is supplied separately through the ordinary Gx vertex declarations.
enum class GxFarTextureFormat : uint8_t { Rgba8, Bc1, Bc2, Bc3 };
inline uint64_t GxFarMipBytes(GxFarTextureFormat format,uint32_t width,uint32_t height) {
    if(!width || !height) return 0;
    uint64_t elements=0,bytes=0;
    switch(format) {
        case GxFarTextureFormat::Rgba8: elements=uint64_t(width)*height;bytes=4;break;
        case GxFarTextureFormat::Bc1: elements=((uint64_t(width)+3)/4)*((uint64_t(height)+3)/4);bytes=8;break;
        case GxFarTextureFormat::Bc2:
        case GxFarTextureFormat::Bc3: elements=((uint64_t(width)+3)/4)*((uint64_t(height)+3)/4);bytes=16;break;
        default:return 0;
    }
    return elements>UINT64_MAX/bytes?0:elements*bytes;
}
struct GxFarMip {
    const uint8_t* data = nullptr;
    size_t bytes = 0; // tightly packed pixels/BC blocks; no conversion or asset I/O
};
struct GxFarResourceRequest {
    const void* vertices = nullptr;
    uint32_t vertexStride = 0, vertexCount = 0;
    const uint16_t* indices = nullptr;
    uint32_t indexCount = 0;
    uint32_t width = 0, height = 0;
    std::vector<GxFarMip> mips;
    bool linearFilter = true, wrapU = false, wrapV = false;
    GxFarTextureFormat format = GxFarTextureFormat::Rgba8;
};
enum class GxFarResourceStatus : uint8_t { Resident, Withdrawn, DeviceExpired };
enum class GxFarResourceResult : uint8_t { Unsupported, Invalid, Budget, AllocationFailure, Unavailable, Ready };

// Owner/render-thread API, including final release. Borrowed Gx objects are
// immutable: do not BufData/BufDestroy/TexDestroy them. Bind immediately through
// PrimVertexFormat/PrimIndex and RsSet(TextureN), retaining the lease. Withdrawal
// invalidates getters immediately; already recorded readers remain fence-owned.
// Leases may outlive device destruction and then report DeviceExpired.
class GxFarResourceLease {
public:
    virtual ~GxFarResourceLease() = default;
    virtual GxFarResourceStatus Status() const = 0;
    virtual CGxBuf* VertexBuffer() const = 0;
    virtual CGxBuf* IndexBuffer() const = 0;
    virtual CGxTex* Texture() const = 0;
    virtual void Withdraw() = 0;
};
struct GxFarResourceAdmission {
    GxFarResourceResult result = GxFarResourceResult::Unsupported;
    std::shared_ptr<GxFarResourceLease> lease;
};
struct GxFarResourceUsage {
    uint64_t sceneBytes = 0, farGeometryBytes = 0;
    uint64_t imageBytes = 0, farImageBytes = 0;
};
#endif
