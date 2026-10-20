// Handwritten terrain receiver using the original CMapRenderChunk register ABI.
// Only the explicitly authored layer/lighting specialisations include this file.
#if !GX_DIRECTIONAL_SHADOWS || !GX_SHADER_VERTEX
#error Terrain directional vertex source requires interface v2 vertex compilation
#endif
#include "shared/DirectionalLighting.slang"
[[vk::binding(0,1)]] cbuffer TerrainRegisters { float4 terrainRegisters[256]; };
struct main_Input {
    [[vk::location(0)]] float4 position:POSITION0;
    [[vk::location(3)]] float3 normal:NORMAL0;
};
struct main_Output {
    float4 position:SV_Position;
    [[vk::location(0)]] float4 classicLight:COLOR0;
    [[vk::location(1)]] float4 classicSpecular:COLOR1;
    [[vk::location(2)]] float4 uv0:TEXCOORD0;
    [[vk::location(3)]] float4 uv1:TEXCOORD1;
    [[vk::location(4)]] float4 uv2:TEXCOORD2;
    [[vk::location(5)]] float4 uv3:TEXCOORD3;
#if GX_TERRAIN_LAYERS == 4
    [[vk::location(6)]] float4 uv4:TEXCOORD4;
#endif
    [[vk::location(7)]] float3 worldPosition:TEXCOORD5;
    [[vk::location(8)]] float3 unshadowedLight:TEXCOORD6;
    [[vk::location(9)]] float3 worldSun:TEXCOORD7;
    [[vk::location(10)]] float fog:FOG;
};
float4 TerrainTransform(float4 p,uint first) {
    return p.x*terrainRegisters[first]+p.y*terrainRegisters[first+1]+p.z*terrainRegisters[first+2]+p.w*terrainRegisters[first+3];
}
float4 TerrainUv(float4 p,uint layer) {
    float2 relative=p.yx-terrainRegisters[23].yx;
    return float4(relative*terrainRegisters[18+layer].yx+terrainRegisters[13+layer].yx,0,0);
}
float3 TerrainPointLight(float3 eye,float3 normal,uint first) {
    float3 relative=terrainRegisters[first].xyz;
    float3 delta=relative.x*terrainRegisters[0].xyz+relative.y*terrainRegisters[1].xyz+relative.z*terrainRegisters[2].xyz-eye;
    float distance=length(delta);if(distance==0)return 0;
    float attenuation=dot(terrainRegisters[first+2].xyz,float3(1,distance,distance*distance));
    float inverse=attenuation==0?1e38:1/attenuation;
    return terrainRegisters[first+1].xyz*saturate(dot(normal,delta/distance))*inverse;
}
main_Output main(main_Input input) {
    main_Output output=(main_Output)0;
    float4 eye=TerrainTransform(float4(input.position.xyz,1),0);
    output.position=TerrainTransform(eye,4);
    float3 normal=input.normal.x*terrainRegisters[0].xyz+input.normal.y*terrainRegisters[1].xyz+input.normal.z*terrainRegisters[2].xyz;
    output.worldSun=terrainRegisters[26].xyz*saturate(dot(terrainRegisters[24].xyz,normal));
    output.unshadowedLight=terrainRegisters[25].xyz;
#if GX_TERRAIN_POINTS
    output.unshadowedLight+=TerrainPointLight(eye.xyz,normal,28)+TerrainPointLight(eye.xyz,normal,31)+TerrainPointLight(eye.xyz,normal,34);
#endif
    output.classicLight=float4(min(output.unshadowedLight+output.worldSun,1)*.5,1);
    output.classicSpecular=0;
    output.worldPosition=GxShadowWorldPosition(eye.xyz);
    output.uv0=TerrainUv(input.position,0);output.uv1=TerrainUv(input.position,1);
    output.uv2=TerrainUv(input.position,2);output.uv3=TerrainUv(input.position,3);
#if GX_TERRAIN_LAYERS == 4
    output.uv4=TerrainUv(input.position,4);
#endif
    output.fog=min(pow(max(eye.z*terrainRegisters[12].x+terrainRegisters[12].y,0),terrainRegisters[12].z),1);
    return output;
}
