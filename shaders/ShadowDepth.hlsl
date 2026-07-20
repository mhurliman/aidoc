// Depth-only shadow caster pass. Transforms position by (world * cascade light view-proj).
// Both matrices are uploaded transposed (same convention as the main shaders).
#define SHADOW_ROOTSIG "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)"

cbuffer Cascade : register(b0) { float4x4 lightViewProj; };
cbuffer Object  : register(b1) { float4x4 world; };

struct VSInput { float3 position : POSITION; };

[RootSignature(SHADOW_ROOTSIG)]
float4 VSMain(VSInput input) : SV_Position
{
    float4 worldPos = mul(float4(input.position, 1.0), world);
    return mul(worldPos, lightViewProj);
}
