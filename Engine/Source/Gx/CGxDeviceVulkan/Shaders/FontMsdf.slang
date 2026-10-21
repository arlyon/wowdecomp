#ifndef GX_HDR_OUTPUT
#define GX_HDR_OUTPUT 0
#endif
#if GX_HDR_OUTPUT
#include "HdrToneMapping.slang"
#endif
// Native Gxu MSDF ABI: the first fixed-function matrix, RGBA distance atlas,
// and (alpha threshold, outline texels, distance range, atlas size).
struct VertexConstants { row_major float4x4 worldViewProjection; };
struct PixelConstants { float4 options; };
[[vk::binding(0, 1)]] ConstantBuffer<VertexConstants> vertexConstants;
[[vk::binding(0, 3)]] ConstantBuffer<PixelConstants> pixelConstants;
[[vk::binding(0, 2)]] Sampler2D<float4> atlas;
struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(4)]] float4 colour : COLOR0;
    [[vk::location(6)]] float2 uv : TEXCOORD0;
};
struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 colour : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1), vertexConstants.worldViewProjection);
    output.colour = input.colour.bgra;
    output.uv = input.uv;
    return output;
}
float median(float3 v) { return max(min(v.r, v.g), min(max(v.r, v.g), v.b)); }
#if GX_FONT_TEMPORAL || GX_HDR_OUTPUT
struct FontOutput {
    [[vk::location(0)]] float4 colour : SV_Target0;
#if GX_FONT_TEMPORAL
    [[vk::location(1)]] float4 motion : SV_Target1;
#endif
#if GX_HDR_OUTPUT
    [[vk::location(2)]] float4 classic : SV_Target2;
#endif
};
[shader("fragment")]
FontOutput pixelMain(VertexOutput input) {
#else
[shader("fragment")]
float4 pixelMain(VertexOutput input) : SV_Target {
#endif
    float4 options = pixelConstants.options;
    float2 footprint = max(fwidth(input.uv), float2(1e-6));
    float screenRange = max(0.5 * dot(float2(options.z / options.w), 1.0 / footprint), 1.0);
    float4 texel = atlas.Sample(input.uv);
    float signedSample = median(texel.rgb);
    float distance = signedSample - 0.5;
    float fill = saturate(distance * screenRange + 0.5);
    float coverage = saturate((distance + options.y / options.z) * screenRange + 0.5);
    // Empty glyphs and saturated exterior gutters never become outline rectangles.
    coverage *= texel.a * (signedSample > 0 ? 1.0 : 0.0);
    // Straight-alpha blending: black outline must not multiply coverage twice.
    float3 colour = input.colour.rgb * (coverage > 0 ? fill / coverage : 0);
    float alpha = input.colour.a * coverage;
    clip(alpha - max(options.x, 1.0 / 255.0));
    #if GX_FONT_TEMPORAL || GX_HDR_OUTPUT
    FontOutput output;
    output.colour = float4(colour, alpha);
    #if GX_HDR_OUTPUT
    output.classic = output.colour;
    #if !GX_HDR_ENCODED
    output.colour.rgb = hdrDecodeLegacy(colour);
    #endif
    #endif
    #if GX_FONT_TEMPORAL
    output.motion = float4(0, 0, -1, 1); // text/shadows are deliberately reactive, not motion-tracked
    #endif
    return output;
    #else
    return float4(colour, alpha);
    #endif
}
