// Asset-free resource/query probe. This fixture declares opaque triangles only;
// it is not the production material/coverage evaluator or RT acceptance gate.
struct ProbeRay { float4 originMin; float4 directionMax; };
struct ProbeResult { uint hit; uint primitive; float distance; uint padding; };
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeRay> rays;
[[vk::binding(2, 0)]] RWStructuredBuffer<ProbeResult> results;
[numthreads(64, 1, 1)]
void computeMain(uint3 thread : SV_DispatchThreadID) {
    uint count, stride;
    rays.GetDimensions(count, stride);
    if (thread.x >= count) return;
    ProbeRay input = rays[thread.x];
    RayDesc ray;
    ray.Origin = input.originMin.xyz;
    ray.TMin = input.originMin.w;
    ray.Direction = input.directionMax.xyz;
    ray.TMax = input.directionMax.w;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            query.CommitNonOpaqueTriangleHit();
    }
    ProbeResult output = (ProbeResult)0;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        output.hit = 1;
        output.primitive = query.CommittedPrimitiveIndex();
        output.distance = query.CommittedRayT();
    }
    results[thread.x] = output;
}
