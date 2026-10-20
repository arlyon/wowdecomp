// Handwritten three/four-layer terrain material. Directional visibility replaces
// the directional portion of authored static shadow modulation; it never dims
// the independently supplied ambient/local contribution or the fog colour.
#if !GX_DIRECTIONAL_SHADOWS || GX_SHADER_VERTEX
#error Terrain directional pixel source requires interface v2 pixel compilation
#endif
#include "shared/DirectionalLighting.slang"
[[vk::binding(0,3)]] cbuffer TerrainPixelRegisters { float4 terrainPixelRegisters[256]; };
[[vk::binding(1,3)]] cbuffer TerrainAlphaTest { float4 terrainAlphaTest; };
[[vk::binding(0,2)]] Sampler2D<float4> layer0;
[[vk::binding(1,2)]] Sampler2D<float4> layer1;
[[vk::binding(2,2)]] Sampler2D<float4> layer2;
#if GX_TERRAIN_LAYERS >= 3
[[vk::binding(3,2)]] Sampler2D<float4> layer3OrWeights;
#endif
#if GX_TERRAIN_LAYERS == 4
[[vk::binding(4,2)]] Sampler2D<float4> layerWeights;
#endif
struct main_Input {
    [[vk::location(0)]] float4 classicLight:COLOR0;
    [[vk::location(1)]] float4 classicSpecular:COLOR1;
    [[vk::location(2)]] float4 uv0:TEXCOORD0;
    [[vk::location(3)]] float4 uv1:TEXCOORD1;
    [[vk::location(4)]] float4 uv2:TEXCOORD2;
#if GX_TERRAIN_LAYERS >= 3
    [[vk::location(5)]] float4 uv3:TEXCOORD3;
#endif
#if GX_TERRAIN_LAYERS == 4
    [[vk::location(6)]] float4 uv4:TEXCOORD4;
#endif
    [[vk::location(7)]] float3 worldPosition:TEXCOORD5;
    [[vk::location(8)]] float3 unshadowedLight:TEXCOORD6;
    [[vk::location(9)]] float3 worldSun:TEXCOORD7;
    [[vk::location(10)]] float fog:FOG;
};
struct main_Output {float4 m_oC0:SV_Target0;float4 classic:SV_Target2;};
main_Output main(main_Input input) {
    main_Output output;
#if GX_TERRAIN_LAYERS == 4
    float4 weights=layerWeights.Sample(input.uv4.xy);
#elif GX_TERRAIN_LAYERS == 3
    float4 weights=layer3OrWeights.Sample(input.uv3.xy);
#else
    float4 weights=layer2.Sample(input.uv2.xy);
#endif
    float4 reflectance=lerp(layer0.Sample(input.uv0.xy),layer1.Sample(input.uv1.xy),weights.x);
#if GX_TERRAIN_LAYERS >= 3
    reflectance=lerp(reflectance,layer2.Sample(input.uv2.xy),weights.y);
#endif
#if GX_TERRAIN_LAYERS == 4
    reflectance=lerp(reflectance,layer3OrWeights.Sample(input.uv3.xy),weights.z);
#endif
    float staticShadow=.7+.3*weights.w;
    float3 classic=reflectance.rgb*staticShadow*input.classicLight.rgb*2;
    classic+=reflectance.a*input.classicSpecular.rgb*weights.w;
    float3 shaded=classic;
    if(gxShadowFrame[6].y!=0 && all(abs(input.worldPosition-gxShadowFrame[4].xyz)<=gxShadowFrame[4].w)){
        float visibility=GxWorldSunVisibility(input.worldPosition);
        float3 light=GxComposeLighting(input.unshadowedLight*staticShadow,0,0,input.worldSun,0,visibility);
        shaded=reflectance.rgb*min(light,1)+reflectance.a*input.classicSpecular.rgb*visibility;
    }
    float3 fog=terrainPixelRegisters[2].xyz;
    output.m_oC0=float4(lerp(fog,shaded,input.fog),1);
    output.classic=float4(lerp(fog,classic,input.fog),1);
    clip(output.m_oC0.a-terrainAlphaTest.x);
    return output;
}
