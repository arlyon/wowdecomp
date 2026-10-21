#ifndef WOW_WORLDCLIENT_FARADTPREPARED_H
#define WOW_WORLDCLIENT_FARADTPREPARED_H
#include "FarAdtProducer.h"
#include <array>

namespace FarAdt {
constexpr size_t PreparedLimit = 8*1024*1024; // subdivision of PartitionLimit, never another pool
constexpr size_t MaterialBytes = 1024*1024;
constexpr size_t UsedTextures = 256*4;
constexpr unsigned BlendChunkSide=64, BlendChunkStride=66, BlendAtlasSide=16*BlendChunkStride;
constexpr size_t BlendOffset(unsigned chunk,unsigned x,unsigned y) {
    return ((chunk/16*BlendChunkStride+1+y)*BlendAtlasSide+chunk%16*BlendChunkStride+1+x)*4;
}
enum class PrepareResult { Pending, Ready, BudgetDeferred, Unsupported, Malformed, ReadError, Cancelled, SourceChanged };
enum class AlbedoEncoding { BC1, BC2, BC3, RGBA8 };
// Native world axes: x decreases with ADT/chunk row, y with column, z up.
// Tightly packed 32-byte vertex: float3 position, float3 authored normal,
// byte4 RGBA MCCV (neutral127,127,127,255), uint16 column/row in half-cells.
// UV = (halfColumn,halfRow)/2; alpha UV = same/16. Layer UV repeats per cell.
// MCCV remains separate from intrinsic sRGB albedo; near shader interprets its
// neutral127 value. No sun, ambient, shadow or dynamic lighting is baked here.
struct PreparedVertex {
    float position[3], normal[3];
    uint8_t shading[4];
    uint16_t halfColumn, halfRow;
};
static_assert(sizeof(PreparedVertex)==32, "neutral vertex ABI");
struct PreparedTexture {
    std::array<char,260> name{};
    uint32_t sourceId=0, flags=0, width=0, height=0, mip=0, offset=0, bytes=0;
    AlbedoEncoding encoding=AlbedoEncoding::RGBA8;
    uint32_t sourceWidth=0, sourceHeight=0;
    bool reduced=false; // sparse nearest reduction only when no bounded authored mip fits
};
// Coverage key is (map,generation,tile key,chunk array index=row*16+column).
struct PreparedChunk {
    uint32_t areaId=0, flags=0, layerCount=0;
    uint16_t holes=0;
    // Tile-global uint16 indices (256*145 vertices fit); no base-vertex draw
    // extension or owner-thread index rebuilding is required. LODs retain ALL
    // 9 edge vertices: coarse removes interior cell centres only. No skirts,
    // cracks or hole bridging; same boundary sequence across all LODs.
    // Levels2/3/4 use step2/4/8 corner patches, recursively refine holes to
    // authored2x2 hole cells; interior patch boundaries sample step2, hole/
    // exterior boundaries step1. Uploaders may compact referenced vertices.
    uint32_t firstIndex[5]{}, indexCount[5]{};
    float lodError[5]{}; // levels2..4 conservative vertical height-range bound
    float coarseError=0;
    std::array<std::array<uint16_t,9>,4> edges{}; // chunk-local top,right,bottom,left
    // MCLY0x40 animated (direction bits0..2, rate bits3..5),0x80 unlit,
    //0x400 specular/reflection variant: never silently consume as static diffuse
    //exactness. GPU consumer must honor these descriptors or report unsupported.
    std::array<Layer,4> layers{}; // textureId indexes PreparedTile::textures
};
struct PreparedTile {
    std::shared_ptr<const void> charge; // final release OWNER THREAD, including readers
    Key key{};
    uint64_t map=0, generation=0, ticket=0;
    uint32_t mapFlags=0, textureCount=0, indexCount=0, materialBytes=0;
    std::array<PreparedVertex,256*145> vertices{};
    std::array<uint16_t,256*(768+384*4)> indices{};
    std::array<PreparedChunk,256> chunks{};
    // GPU-ready RGBA atlas of64x64 chunks, each with a duplicated1px border.
    // Border padding preserves per-chunk clamp/bilinear sampling in one image.
    // RGB contains the three authored opacity
    // channels; A is authored sun visibility (MCSH bit1 ->0, absent ->255).
    // Big alpha uses (1-sum(a),a1,a2,a3); legacy uses sequential over blending.
    // Near source: UnpackAlphaShadowBitsFixed8888Mip0 / s_shadowAlpha and
    // UnpackAlphaBits{Unfixed,Fixed}4444Mip0. Legacy edge63 duplicates edge62
    // unless MCNK0x8000. All decoding happens on the preparation worker.
    std::array<uint8_t,BlendAtlasSide*BlendAtlasSide*4> blend{};
    std::array<PreparedTexture,UsedTextures> textures{};
    // Owned selected authored BLP mip per layer, sRGB RGB, straight alpha.
    // BC blocks retained losslessly; palette/raw decoded RGBA8. Explicit mip
    // number/resolution; no claim of full source resolution or generated mips.
    std::array<uint8_t,MaterialBytes> albedo{};
};
// Pure CPU; caller precharges output and keeps immutable input alive.
PrepareResult PrepareGeometry(const Tile&, PreparedTile&);
struct MipRequest { uint32_t offset=0, bytes=0, fileBytes=0, headerBytes=0; bool sparse=false; };
PrepareResult PrepareTextureHeader(const uint8_t* header, size_t bytes, uint32_t fileBytes,
    uint32_t textureCount, PreparedTexture&, MipRequest&);
PrepareResult PrepareTextureMip(const uint8_t* header, const uint8_t* mip, size_t bytes,
    PreparedTexture&, uint8_t* output, size_t capacity);
// Bounded sparse fallback for legal assets without a fitting authored mip.
// One source block/texel (plus separate palette alpha byte if present) per read.
MipRequest TextureSampleRequest(const uint8_t* header,const PreparedTexture&,const MipRequest&,
    uint32_t sample,bool paletteAlpha);
PrepareResult PrepareTextureSample(const uint8_t* header,const uint8_t* bytes,size_t count,
    const PreparedTexture&,uint32_t sample,uint8_t paletteIndex,uint8_t* rgba);
// Animation uses LIVE World::texVect, not wall clock or baked noon. Pass the
// matching direction's accumulated vector; reset/wrap behavior stays near-owned.
std::array<float,2> LayerUvOffset(uint32_t flags, std::array<float,2> worldTexVector);
}
#endif
