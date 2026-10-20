// Native-resolution scene-only resolve. Never bind UI-composited colour here.
// Vulkan ABI: set 0: sampled images 0=current RGBA, 1=previous resolved RGBA,
// 2=motion RGBA FLOAT, 3=previous device depth (R); sampler 4; uniform buffer 5.
// Draw 3 vertices, no vertex buffers; both compiled SPIR-V entry names are main.
// Output is a distinct colour attachment (never aliases any input), blending OFF.
// Inputs in SHADER_READ_ONLY_OPTIMAL; caller owns barriers/history ping-pong.
// All images single-sampled, same output extent, mip 0. Colour views must NOT be
// sRGB: preserve the caller's classic numeric colour representation (no transfer).
// Motion xy=current-minus-previous UNJITTERED top-down UV; z=expected previous
// device depth after previous viewport mapping [0,1], -1 invalid; w=reactivity.
// History grid is the unjittered output grid. Jitter affects scene sampling only.
// Positive-height Vulkan viewport; UV=(pixel+0.5)/extent, top-left origin.
// Sampler: nearest, clamp-to-edge, mip 0. All filtering uses explicit Load taps;
// binding 4 is consequently unused/optimized out but retained in the host layout.
// History bilinear taps are individually validated, never hardware depth-filtered.
// UBO std140, 80 bytes, offsets 0/16/32/48/64, each member 16-byte aligned:
// extentValidity=(width,height,historyValid,reserved=0);
// viewport / previousViewport=(inclusive min x,y, EXCLUSIVE max x,y), pixels;
// accumulation=(historyWeight,deviceDepthTolerance,0,0); reserved=(0,0,0,0).
// Host supplies valid nonempty in-image viewports. Weight clamped [0,.95],
// tolerance [0,.01] in NORMALIZED DEVICE DEPTH, NOT linear/view-space distance.
// This prototype's fixed device-depth tolerance is far-plane sensitive: distant
// surfaces may alias in depth. It is not a production linear-depth solution.
[[vk::binding(0,0)]] Texture2D<float4> currentScene;
[[vk::binding(1,0)]] Texture2D<float4> previousScene;
[[vk::binding(2,0)]] Texture2D<float4> motionScene;
[[vk::binding(3,0)]] Texture2D<float> previousDepth;
[[vk::binding(4,0)]] SamplerState pointClamp;
[[vk::binding(5,0)]] cbuffer ResolveConstants {
    uint4 extentValidity;
    int4 viewport;
    int4 previousViewport;
    float4 accumulation;
    float4 reserved;
};

float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2 - 1, 0, 1);
}
bool inside(int2 p, int4 bounds) {
    return all(p >= bounds.xy) && all(p < bounds.zw);
}
float4 pixelMain(float4 position : SV_Position) : SV_Target0 {
    int2 p = int2(position.xy);
    float4 fresh = currentScene.Load(int3(p,0));
    if (extentValidity.z == 0 || !inside(p,viewport)) return fresh;
    float4 motion = motionScene.Load(int3(p,0));
    if (!all(isfinite(motion)) || motion.z < 0 || motion.z > 1 ||
        motion.w < 0 || motion.w >= 1 || !all(isfinite(accumulation.xy))) return fresh;
    float weight = clamp(accumulation.x,0.0,0.95) * (1-motion.w);
    if (weight <= 0) return fresh;
    float2 previousUV = (float2(p)+0.5)/float2(extentValidity.xy)-motion.xy;
    if (!all(isfinite(previousUV)) ||
        any(previousUV < float2(previousViewport.xy)/float2(extentValidity.xy)) ||
        any(previousUV >= float2(previousViewport.zw)/float2(extentValidity.xy))) return fresh;
    float2 samplePosition = previousUV * float2(extentValidity.xy)-0.5;
    int2 base = int2(floor(samplePosition));
    float2 fraction = frac(samplePosition);
    float3 history = 0;
    // Every contributing bilinear tap must be in the previous scene viewport,
    // finite, and consistent with EXPECTED PREVIOUS depth, not current depth.
    for (int y=0; y<2; ++y) for (int x=0; x<2; ++x) {
        float tapWeight = (x != 0 ? fraction.x : 1-fraction.x)*(y != 0 ? fraction.y : 1-fraction.y);
        if (tapWeight <= 0) continue;
        int2 tap = base+int2(x,y);
        if (!inside(tap,previousViewport)) return fresh;
        float depth = previousDepth.Load(int3(tap,0));
        if (!isfinite(depth) || depth < 0 || depth > 1 ||
            abs(depth-motion.z) > clamp(accumulation.y,0.0,0.01)) return fresh;
        float3 colour = previousScene.Load(int3(tap,0)).rgb;
        if (!all(isfinite(colour))) return fresh;
        history += tapWeight*colour;
    }
    float3 low = fresh.rgb, high = fresh.rgb;
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
        int2 tap = clamp(p+int2(x,y),viewport.xy,viewport.zw-1);
        float3 colour = currentScene.Load(int3(tap,0)).rgb;
        if (!all(isfinite(colour))) return fresh;
        low = min(low,colour); high = max(high,colour);
    }
    return float4(lerp(fresh.rgb,clamp(history,low,high),weight),fresh.a);
}
