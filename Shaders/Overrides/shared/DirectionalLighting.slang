// Explicit lighting inputs: visibility is never applied to ambient/local/emission.
float3 GxComposeLighting(float3 ambient,float3 local,float3 emission,
    float3 directionalDiffuse,float3 directionalSpecular,float visibility) {
    return ambient+local+emission+(directionalDiffuse+directionalSpecular)*visibility;
}
#if GX_DIRECTIONAL_SHADOWS
// Interface v2: view-inverse rows, camera/region, world light/distance,
// then query bias/enabled/reserved. This is not a memcpy of CM2Lighting.
[[vk::binding(2,4)]] cbuffer GxDirectionalFrame { float4 gxShadowFrame[7]; };
float3 GxShadowWorldPosition(float3 eyePosition) {
    return mul(float4(eyePosition,1),float4x4(gxShadowFrame[0],gxShadowFrame[1],gxShadowFrame[2],gxShadowFrame[3])).xyz;
}
#if !GX_SHADER_VERTEX
#include "shared/ShadowCoverage.slang"
float GxWorldSunVisibility(float3 worldPosition) {
    if(gxShadowFrame[6].y==0 || any(abs(worldPosition-gxShadowFrame[4].xyz)>gxShadowFrame[4].w))return 1;
    return GxDirectionalVisibility(worldPosition,gxShadowFrame[5].xyz,gxShadowFrame[5].w,gxShadowFrame[6].x);
}
#endif
#endif
