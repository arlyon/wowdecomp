// Output-grid temporal reconstruction; NOT the native 80-byte resolve ABI.
// Set 0: current internal colour 0, previous OUTPUT colour 1, internal motion 2,
// previous OUTPUT device depth 3, constants 5, current INTERNAL device depth 6.
// Constants are 128 bytes: native prefix, reserved=(internal jitter xy, temporal,0),
// renderExtent=(internal width,height,0,0), renderViewport=(min xy,exclusive max xy),
// previousJitter=(submitted internal jitter xy,0,0).
// Separate output/history attachment. Explicit taps, no sampler/filter capability.
// Motion is full-image unjittered UV; z is expected previous mapped device depth.
// Depth tolerance is fixed NORMALIZED device depth, not a linear-depth solution.
[[vk::binding(0,0)]] Texture2D<float4> currentScene;
[[vk::binding(1,0)]] Texture2D<float4> previousScene;
[[vk::binding(2,0)]] Texture2D<float4> motionScene;
[[vk::binding(3,0)]] Texture2D<float> previousDepth;
[[vk::binding(6,0)]] Texture2D<float> currentDepth;
[[vk::binding(5,0)]] cbuffer UpscaleConstants {
    uint4 extentValidity;
    int4 viewport;
    int4 previousViewport;
    float4 accumulation;
    float4 reconstruction;
    uint4 renderExtent;
    int4 renderViewport;
    float4 previousJitter;
};
float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2,id & 2);
    return float4(uv*2-1,0,1);
}
bool inside(int2 p,int4 bounds) { return all(p >= bounds.xy) && all(p < bounds.zw); }
bool validMotion(float4 m) {
    return all(isfinite(m)) && m.z >= 0 && m.z <= 1 && m.w >= 0 && m.w < 1;
}
float4 spatial(float2 position) {
    int2 base = int2(floor(position));
    float2 f = frac(position);
    float4 result = 0;
    for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        int2 tap = clamp(base+int2(x,y),renderViewport.xy,renderViewport.zw-1);
        float w = (x != 0 ? f.x : 1-f.x)*(y != 0 ? f.y : 1-f.y);
        result += w*currentScene.Load(int3(tap,0));
    }
    return result;
}
float4 pixelMain(float4 position : SV_Position) : SV_Target0 {
    int2 p = int2(position.xy);
    float2 uv = (float2(p)+.5)/float2(extentValidity.xy);
    float2 unjittered = uv*float2(renderExtent.xy)-.5;
    float4 fallback = spatial(unjittered);
    if (!inside(p,viewport) || reconstruction.z == 0) return fallback;
    int2 probe = clamp(int2(floor(unjittered+.5)),renderViewport.xy,renderViewport.zw-1);
    // Unknown/deforming geometry was deliberately rasterised WITHOUT jitter.
    // Never apply the frame jitter to these regions or reuse their history.
    if (!validMotion(motionScene.Load(int3(probe,0)))) return fallback;
    float2 samplePosition = unjittered+reconstruction.xy;
    int2 nearest = int2(floor(samplePosition+.5));
    if (!inside(nearest,renderViewport)) return fallback;
    float4 motion = motionScene.Load(int3(nearest,0));
    float depth = currentDepth.Load(int3(nearest,0));
    if (!validMotion(motion) || !isfinite(depth) || depth < 0 || depth > 1) return fallback;
    float4 fresh = spatial(samplePosition);
    if (extentValidity.z == 0 || !all(isfinite(fresh))) return fresh;
    // Reject mixed jitter/unjittered footprints and current-depth discontinuities.
    // This also prevents a valid foreground velocity from dragging unknown sky.
    int2 base = int2(floor(samplePosition));
    for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        int2 tap = base+int2(x,y);
        if (!inside(tap,renderViewport)) return fresh;
        float4 m = motionScene.Load(int3(tap,0));
        float d = currentDepth.Load(int3(tap,0));
        if (!validMotion(m)) return fallback;
        if (!isfinite(d) || abs(d-depth) > accumulation.y ||
            abs(m.z-motion.z) > accumulation.y || length(m.xy-motion.xy)*max(extentValidity.x,extentValidity.y) > 1)
            return fresh;
    }
    float2 previousPosition = (uv-motion.xy)*float2(extentValidity.xy)-.5;
    if (!all(isfinite(previousPosition))) return fresh;
    int2 historyBase = int2(floor(previousPosition));
    float2 f = frac(previousPosition);
    float3 history = 0;
    for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        float w = (x != 0 ? f.x : 1-f.x)*(y != 0 ? f.y : 1-f.y);
        if (w <= 0) continue;
        int2 tap = historyBase+int2(x,y);
        if (!inside(tap,previousViewport)) return fresh;
        // Colour history is unjittered/output-sized; stored previous depth is
        // nearest reconstructed from the previous jittered INTERNAL attachment.
        int2 depthTap = int2(floor(float2(tap)+.5+
            previousJitter.xy*float2(extentValidity.xy)/float2(renderExtent.xy)));
        if (!inside(depthTap,previousViewport)) return fresh;
        float d = previousDepth.Load(int3(depthTap,0));
        float3 c = previousScene.Load(int3(tap,0)).rgb;
        if (!isfinite(d) || d < 0 || d > 1 || abs(d-motion.z) > accumulation.y || !all(isfinite(c))) return fresh;
        history += w*c;
    }
    float3 low = fresh.rgb, high = fresh.rgb;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        int2 tap = clamp(nearest+int2(x,y),renderViewport.xy,renderViewport.zw-1);
        float3 c = currentScene.Load(int3(tap,0)).rgb;
        if (!all(isfinite(c))) return fresh;
        low = min(low,c); high = max(high,c);
    }
    // Accumulate actual jittered point samples on the OUTPUT grid. Proximity
    // weighting favours samples near this output pixel instead of permanently
    // integrating a low-resolution bilinear tent (which cannot recover detail).
    float2 distance = samplePosition-float2(nearest);
    float confidence = exp(-12*dot(distance,distance));
    float currentWeight = lerp(1,clamp(.15*confidence,.005,.15),1-motion.w);
    float3 sample = currentScene.Load(int3(nearest,0)).rgb;
    return float4(lerp(clamp(history,low,high),sample,currentWeight),fresh.a);
}
