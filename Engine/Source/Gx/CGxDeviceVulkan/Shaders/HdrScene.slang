#include "HdrToneMapping.slang"
[[vk::binding(0,0)]] Texture2D<float4> scene;
[[vk::binding(1,0)]] SamplerState sceneSampler;
[[vk::binding(2,0)]] StructuredBuffer<float4> gradingLut;
[[vk::push_constant]] cbuffer Options { uint operation; float exposureEV; float2 outputSize; uint4 lutInfo; };
float3 gradeLut(float3 c) {
    uint n=lutInfo.x;if(n<2) return c;
    float3 p=saturate(c)*(n-1);uint3 lo=uint3(floor(p));uint3 hi=min(lo+1,n-1);float3 f=p-lo;
    float3 result=0;
    [unroll] for(uint z=0;z<2;++z) [unroll] for(uint y=0;y<2;++y) [unroll] for(uint x=0;x<2;++x) {
        uint3 v=uint3(x!=0?hi.x:lo.x,y!=0?hi.y:lo.y,z!=0?hi.z:lo.z);
        float3 w=float3(x!=0?f.x:1-f.x,y!=0?f.y:1-f.y,z!=0?f.z:1-f.z);
        result+=gradingLut[v.x+n*(v.y+n*v.z)].rgb*w.x*w.y*w.z;
    }
    return result;
}
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOutput vertexMain(uint vertex : SV_VulkanVertexID) {
    VertexOutput o; o.uv=float2(vertex==1?2:0,vertex==2?2:0);
    o.position=float4(o.uv*2-1,0,1); return o;
}
float4 pixelMain(VertexOutput input) : SV_Target0 {
    float4 c=scene.SampleLevel(sceneSampler,input.position.xy/outputSize,0);
    if(operation==0) return float4(hdrDecodeLegacy(c.rgb),c.a);
    float4 mapped=hdrToneMapSdr(c,exposureEV);return float4(gradeLut(mapped.rgb),mapped.a);
}
