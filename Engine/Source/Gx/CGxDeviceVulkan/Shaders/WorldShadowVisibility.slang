// Shared world AS visibility diagnostic. Primary raster depth/coverage is kept;
// this view displays the query result, not a substitute for directional lighting.
struct TriangleCoverage {
    float4 uv01;
    float4 uv2OpacityThreshold;
    uint4 flags; // texture, coverage mode, two-sided, reserved
};
[[vk::binding(0,0)]] RaytracingAccelerationStructure world;
[[vk::binding(1,0)]] cbuffer Frame {
    row_major float4x4 inverseViewProjection;
    float4 cameraRegion;
    float4 lightDistance;
    float4 depthRange;
    uint4 viewRect;
    uint4 dimensions;
};
[[vk::binding(2,0)]] StructuredBuffer<TriangleCoverage> triangles;
[[vk::binding(3,0)]] Texture2D<float> rasterDepth;
[[vk::binding(4,0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> visibility;
[[vk::binding(5,0)]] Sampler2D<float4> alphaTextures[];
bool Covered(inout RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query) {
    TriangleCoverage surface=triangles[query.CandidatePrimitiveIndex()];
    if(!surface.flags.z && !query.CandidateTriangleFrontFace())return false;
    if(surface.flags.y==0)return true; // Opaque never reads diffuse/emission.
    if(surface.flags.y!=1 && surface.flags.y!=2)return false;
    float2 bary=query.CandidateTriangleBarycentrics();
    float2 uv=surface.uv01.xy*(1-bary.x-bary.y)+surface.uv01.zw*bary.x+surface.uv2OpacityThreshold.xy*bary.y;
    float alpha=alphaTextures[NonUniformResourceIndex(surface.flags.x)].SampleLevel(uv,0).a;
    return alpha*surface.uv2OpacityThreshold.z>=surface.uv2OpacityThreshold.w;
}
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
    RayDesc ray;ray.Direction=normalize(lightDistance.xyz);ray.Origin=position.xyz+ray.Direction*.015;
    ray.TMin=.015;ray.TMax=lightDistance.w;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(world,RAY_FLAG_NONE,0xff,ray);
    while(query.Proceed())if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && Covered(query))query.CommitNonOpaqueTriangleHit();
    float value=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?.12:1;
    visibility[pixel]=float4(value,value,value,1);

}
