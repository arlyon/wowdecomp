// Unlit Gx fixed-function primitives. Preserve the D3D9 texture-stage contract
// used by minimap masks, sky gradients, clouds and ordinary coloured UI geometry.
#ifndef GX_TEMPORAL
#define GX_TEMPORAL 0
#endif
#ifndef GX_HDR_OUTPUT
#define GX_HDR_OUTPUT 0
#endif
// Legacy pixel programs do not necessarily consume the fixed vertex FOG ABI.
#ifndef GX_FOG_EXPORT
#define GX_FOG_EXPORT 1
#endif
#if GX_HDR_OUTPUT
#include "HdrToneMapping.slang"
#endif

struct VertexConstants
{
    row_major float4x4 worldViewProjection;
    row_major float4x4 worldView;
    row_major float4x4 normalTransform;
    row_major float4x4 textureTransform[8];
    float4 textureControl[8]; // coordinate index, texgen mode, transform mode, enabled
    float4 fog; // enabled, linear start/end, reserved
};
struct PixelConstants
{
    float4 options; // alpha threshold
    float4 stage[8]; // enabled, colour operation, alpha operation
    float4 fog; // encoded RGB fog colour, enabled
};
[[vk::binding(0, 1)]] ConstantBuffer<VertexConstants> vertexConstants;
[[vk::binding(0, 3)]] ConstantBuffer<PixelConstants> pixelConstants;
[[vk::binding(0, 2)]] Sampler2D<float4> texture0;
[[vk::binding(1, 2)]] Sampler2D<float4> texture1;
[[vk::binding(2, 2)]] Sampler2D<float4> texture2;
[[vk::binding(3, 2)]] Sampler2D<float4> texture3;
[[vk::binding(4, 2)]] Sampler2D<float4> texture4;
[[vk::binding(5, 2)]] Sampler2D<float4> texture5;
[[vk::binding(6, 2)]] Sampler2D<float4> texture6;
[[vk::binding(7, 2)]] Sampler2D<float4> texture7;

#if GX_TEMPORAL
[[vk::binding(1, 1)]] ConstantBuffer<VertexConstants> previousVertexConstants;
// xy clip jitter; zw viewport extent / image extent (motion only).
[[vk::binding(2, 1)]] cbuffer TemporalJitter { float4 temporalJitter; };
[[vk::binding(2, 3)]] cbuffer TemporalOptions { float4 temporalOptions; };
#endif

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float4 colour : COLOR0;
    [[vk::location(6)]] float2 uv[8] : TEXCOORD0;
};
struct VertexOutput
{
    float4 position : SV_Position;
#if GX_TEMPORAL
    [[vk::location(11)]] float4 currentClip : TEXCOORD8;
    [[vk::location(12)]] float4 previousClip : TEXCOORD9;
#endif
    [[vk::location(0)]] float4 colour : COLOR0;
    [[vk::location(1)]] float4 secondaryColour : COLOR1;
    [[vk::location(2)]] float4 uv[8] : TEXCOORD0;
#if GX_FOG_EXPORT
    [[vk::location(10)]] float fog : FOG;
#endif
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output = (VertexOutput)0;
    output.position = mul(float4(input.position, 1.0), vertexConstants.worldViewProjection);
#if GX_TEMPORAL
    output.currentClip = output.position;
    output.previousClip = mul(float4(input.position, 1.0), previousVertexConstants.worldViewProjection);
    output.currentClip.xy *= temporalJitter.zw;
    output.previousClip.xy *= temporalJitter.zw;
    output.position.xy += temporalJitter.xy * output.position.w;
#endif
    output.colour = input.colour.bgra;
    float3 eyePosition = mul(float4(input.position, 1.0), vertexConstants.worldView).xyz;
#if GX_FOG_EXPORT
    output.fog = 1.0;
    if (vertexConstants.fog.x != 0.0)
    {
        float distance = abs(eyePosition.z);
        float range = vertexConstants.fog.z - vertexConstants.fog.y;
        output.fog = range > 0.0 ? saturate((vertexConstants.fog.z - distance) / range) :
            (distance < vertexConstants.fog.z ? 1.0 : 0.0);
    }
#endif
    float3 eyeNormal = mul(float4(input.normal, 0.0), vertexConstants.normalTransform).xyz;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 control = vertexConstants.textureControl[i];
        if (control.w == 0.0) continue;
        float4 coordinate = float4(input.uv[int(control.x)], 0.0, 1.0);
        int generation = int(control.y);
        if (generation >= 1 && generation <= 3) coordinate = float4(eyePosition, 1.0);
        else if (generation == 5) coordinate = float4(eyeNormal, 1.0);
        else if (generation == 4 || generation == 6)
            coordinate = float4(reflect(normalize(eyePosition), normalize(eyeNormal)), 1.0);
        coordinate = mul(coordinate, vertexConstants.textureTransform[i]);
        if (int(control.z) == 2)
            coordinate.xy /= coordinate.z;
        output.uv[i] = coordinate;
    }
    return output;
}

// Static bindings avoid requiring sampled-image-array dynamic indexing.
float4 sampleStage(int stage, float2 uv)
{
    switch (stage)
    {
        case 0: return texture0.Sample(uv);
        case 1: return texture1.Sample(uv);
        case 2: return texture2.Sample(uv);
        case 3: return texture3.Sample(uv);
        case 4: return texture4.Sample(uv);
        case 5: return texture5.Sample(uv);
        case 6: return texture6.Sample(uv);
        default: return texture7.Sample(uv);
    }
}
float4 combine(float4 texture, float4 current, float4 diffuse, int operation)
{
    switch (operation)
    {
        case 0: return saturate(texture * current);
        case 1: return saturate(2.0 * texture * current);
        case 2: return saturate(texture + current);
        case 3: return current;
        case 4: return lerp(texture, current, current.a);
        default: return lerp(current, texture, diffuse.a);
    }
}

#if GX_TEMPORAL || GX_HDR_OUTPUT
struct FixedOutput {
    [[vk::location(0)]] float4 colour : SV_Target0;
#if GX_TEMPORAL
    [[vk::location(1)]] float4 motion : SV_Target1;
#endif
#if GX_HDR_OUTPUT
    [[vk::location(2)]] float4 classic : SV_Target2;
#endif
};
[shader("fragment")]
FixedOutput pixelMain(VertexOutput input)
#else
[shader("fragment")]
float4 pixelMain(VertexOutput input) : SV_Target
#endif
{
    float4 colour = input.colour;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float4 stage = pixelConstants.stage[i];
        if (stage.x == 0.0) break;
        float4 texel = sampleStage(i, input.uv[i].xy);
        float4 previous = colour;
        colour.rgb = combine(texel, previous, input.colour, int(stage.y)).rgb;
        colour.a = combine(texel, previous, input.colour, int(stage.z)).a;
    }
    clip(colour.a - pixelConstants.options.x);
#if GX_FOG_EXPORT
    if (pixelConstants.fog.w != 0.0)
        colour.rgb = lerp(pixelConstants.fog.rgb, colour.rgb, saturate(input.fog));
#endif
#if GX_TEMPORAL || GX_HDR_OUTPUT
    FixedOutput output;
    output.colour = colour;
#if GX_HDR_OUTPUT
    output.classic = colour;
    output.colour.rgb = hdrDecodeLegacy(colour.rgb);
#endif
#if GX_TEMPORAL
    output.motion = float4(0, 0, -1, saturate(temporalOptions.y));
    if (temporalOptions.x > 0 && input.currentClip.w > 0.00001 && input.previousClip.w > 0.00001 &&
        all(isfinite(input.currentClip)) && all(isfinite(input.previousClip)))
    {
        float2 delta = input.currentClip.xy / input.currentClip.w - input.previousClip.xy / input.previousClip.w;
        float depth = lerp(temporalOptions.z, temporalOptions.w, input.previousClip.z / input.previousClip.w);
        if (all(isfinite(delta)) && all(abs(delta) <= 4) && isfinite(depth) && depth >= 0 && depth <= 1)
            output.motion = float4(delta * float2(0.5, -0.5), depth, saturate(temporalOptions.y));
    }
#endif
    return output;
#else
    return colour;
#endif
}
