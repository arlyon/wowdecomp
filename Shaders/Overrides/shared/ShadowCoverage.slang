// Original handwritten shared triangle coverage for world visibility queries.
// The native frame owns these descriptors until its original submission fence.
#ifndef GX_SHADOW_DESCRIPTOR_SET
#define GX_SHADOW_DESCRIPTOR_SET 4
#endif
#ifndef GX_SHADOW_TRIANGLE_BINDING
#define GX_SHADOW_TRIANGLE_BINDING 1
#endif
#ifndef GX_SHADOW_TEXTURE_BINDING
#define GX_SHADOW_TEXTURE_BINDING 3
#endif
struct GxShadowTriangle {
    float4 uv01;
    float4 uv2OpacityThreshold;
    uint4 flags; // texture, closed coverage mode, two-sided, reserved
};
[[vk::binding(0,GX_SHADOW_DESCRIPTOR_SET)]] RaytracingAccelerationStructure gxShadowWorld;
[[vk::binding(GX_SHADOW_TRIANGLE_BINDING,GX_SHADOW_DESCRIPTOR_SET)]] StructuredBuffer<GxShadowTriangle> gxShadowTriangles;
[[vk::binding(GX_SHADOW_TEXTURE_BINDING,GX_SHADOW_DESCRIPTOR_SET)]] Sampler2D<float4> gxShadowTextures[];
bool GxShadowCovered(inout RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query) {
    GxShadowTriangle surface=gxShadowTriangles[query.CandidatePrimitiveIndex()];
    if(!surface.flags.z && !query.CandidateTriangleFrontFace())return false;
    if(surface.flags.y==0)return true; // Opaque never samples diffuse/emission.
    if(surface.flags.y!=1 && surface.flags.y!=2)return false;
    float2 bary=query.CandidateTriangleBarycentrics();
    float2 uv=surface.uv01.xy*(1-bary.x-bary.y)+surface.uv01.zw*bary.x+surface.uv2OpacityThreshold.xy*bary.y;
    float alpha=gxShadowTextures[NonUniformResourceIndex(surface.flags.x)].SampleLevel(uv,0).a;
    return alpha*surface.uv2OpacityThreshold.z>=surface.uv2OpacityThreshold.w;
}
// Returns only directional visibility. Ambient/local/emission are not inputs.
float GxDirectionalVisibility(float3 position,float3 direction,float maximumDistance,float bias) {
    RayDesc ray;ray.Direction=normalize(direction);ray.Origin=position+ray.Direction*bias;
    ray.TMin=bias;ray.TMax=maximumDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gxShadowWorld,RAY_FLAG_NONE,0xff,ray);
    while(query.Proceed())
        if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && GxShadowCovered(query))query.CommitNonOpaqueTriangleHit();
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?0:1;
}
