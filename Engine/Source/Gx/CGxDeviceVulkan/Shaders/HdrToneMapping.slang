// Authored scene-linear prototype; not applied to recovered legacy shaders.
// Keep the numerical contract in sync with HdrToneMapping.h.
float hdrToneMapLinear(float value, float exposureEV) {
    if (!isfinite(value) || value <= 0) return 0;
    float scale = exp2(exposureEV);
    return value <= .5 / scale ? value * scale : 1 - .25 / value / scale;
}
float hdrEncodeSdr(float linear) {
    return linear <= .0031308 ? linear * 12.92 : 1.055 * pow(linear, 1.0 / 2.4) - .055;
}
float4 hdrToneMapSdr(float4 scene, float exposureEV) {
    float3 mapped = float3(hdrToneMapLinear(scene.r, exposureEV),
        hdrToneMapLinear(scene.g, exposureEV), hdrToneMapLinear(scene.b, exposureEV));
    return float4(hdrEncodeSdr(mapped.r), hdrEncodeSdr(mapped.g), hdrEncodeSdr(mapped.b), scene.a);
}

float hdrDecodeChannel(float c) { if (!isfinite(c)) return 0; c=max(c,0); return c<=.04045 ? c/12.92 : pow((c+.055)/1.055,2.4); }
float3 hdrDecodeLegacy(float3 c) { return float3(hdrDecodeChannel(c.r),hdrDecodeChannel(c.g),hdrDecodeChannel(c.b)); }
