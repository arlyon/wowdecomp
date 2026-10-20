// Shared world AS visibility diagnostic. Primary raster depth/coverage is kept;
// this view displays the query result, not a substitute for directional lighting.
#define GX_SHADOW_DESCRIPTOR_SET 0
#define GX_SHADOW_TRIANGLE_BINDING 2
#define GX_SHADOW_TEXTURE_BINDING 5
#include "shared/ShadowCoverage.slang"
[[vk::binding(1,0)]] cbuffer Frame {
    row_major float4x4 inverseViewProjection;
    float4 cameraRegion;
    float4 lightDistance;
    float4 depthRange;
    uint4 viewRect;
    uint4 dimensions;
};
[[vk::binding(3,0)]] Texture2D<float> rasterDepth;
[[vk::binding(4,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> visibility;
[numthreads(8,8,1)]
void computeMain(uint3 thread:SV_DispatchThreadID) {
    uint2 pixel=thread.xy;if(any(pixel>=dimensions.xy))return;
    float depth=rasterDepth.Load(int3(pixel,0));
    if(depth>=1 || any(pixel<viewRect.xy) || any(pixel>=viewRect.xy+viewRect.zw)) {
        visibility[pixel]=float4(.15,.2,.3,1);return;
    }
    float2 uv=(float2(pixel)-float2(viewRect.xy)+.5)/float2(viewRect.zw);
    float ndcDepth=(depth-depthRange.x)/(depthRange.y-depthRange.x);
    float4 position=mul(float4(uv.x*2-1,1-uv.y*2,ndcDepth,1),inverseViewProjection);
    position.xyz/=position.w;
    if(any(abs(position.xyz-cameraRegion.xyz)>cameraRegion.w)) {
        visibility[pixel]=float4(.25,.25,.25,1);return;
    }
    float value=lerp(.12,1,GxDirectionalVisibility(position.xyz,lightDistance.xyz,lightDistance.w,.015));
    visibility[pixel]=float4(value,value,value,1);

}
