// Legacy unlit Gx primitives, expressed as maintained source rather than
// synthesised bytecode. Bindings match the translated-shader descriptor layout.
struct VertexConstants
{
    row_major float4x4 worldViewProjection;
};
struct PixelConstants
{
    float4 options; // alpha threshold, texture enabled, unused, unused
};
[[vk::binding(0, 1)]] ConstantBuffer<VertexConstants> vertexConstants;
[[vk::binding(0, 3)]] ConstantBuffer<PixelConstants> pixelConstants;
[[vk::binding(0, 2)]] Sampler2D<float4> baseTexture;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(4)]] float4 colour : COLOR0;
    [[vk::location(6)]] float2 uv : TEXCOORD0;
};
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 colour : COLOR0;
    [[vk::location(2)]] float4 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), vertexConstants.worldViewProjection);
    output.colour = input.colour.bgra; // Gx ARGB colours arrive in BGRA byte order.
    output.uv = float4(input.uv, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 pixelMain(VertexOutput input) : SV_Target
{
    float4 colour = input.colour;
    if (pixelConstants.options.y != 0.0)
        colour *= baseTexture.Sample(input.uv.xy);
    clip(colour.a - pixelConstants.options.x);
    return colour;
}
