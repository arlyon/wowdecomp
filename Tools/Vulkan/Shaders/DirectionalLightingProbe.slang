#define GX_DIRECTIONAL_SHADOWS 1
#define GX_SHADER_VERTEX 0
#include "shared/DirectionalLighting.slang"
[[vk::binding(0,0)]] RWStructuredBuffer<float4> results;
[numthreads(16,1,1)]
void computeMain(uint3 thread:SV_DispatchThreadID) {
    uint i=thread.x;if(i>=16)return;
    float3 position=float3(i*2+.25,.25,i==10?1:(i==11?-.005:-1));
    float3 direction=float3(0,0,i==10?-1:1);
    float distance=i==12?.5:gxShadowFrame[5].w;
    float v=GxDirectionalVisibility(position,direction,distance,gxShadowFrame[6].x);
    float3 ambient=float3(.1,.2,.3),local=float3(.4,.5,.6),emission=float3(.7,.8,.9);
    float3 diffuse=float3(.25,.125,.375)*gxShadowFrame[6].z;
    float3 specular=float3(.0625,.03125,.015625)*gxShadowFrame[6].z;
    GxShadowTriangle t=gxShadowTriangles[i];
    results[i*7+6]=float4(gxShadowTextures[NonUniformResourceIndex(t.flags.x)].SampleLevel(t.uv01.xy,0).a,t.flags.x,t.flags.y,t.uv2OpacityThreshold.w);
    results[i*7+0]=float4(ambient,v);
    results[i*7+1]=float4(local,v);
    results[i*7+2]=float4(emission,v);
    results[i*7+3]=float4(diffuse*v,v);
    results[i*7+4]=float4(specular*v,v);
    results[i*7+5]=float4(GxComposeLighting(ambient,local,emission,diffuse,specular,v),v);
}
