
#include "WaterCommon.hlsli"

ConstantBuffer<WaterConstants> Water : register(b1);

Texture2D<float>  HeightMap      : register(t0);
Texture2D<float2> GradientMap    : register(t1);
Texture2D<float2> DisplacementMap: register(t2);

SamplerState LinearSampler : register(s0);

struct HsPatchOutput
{
    float TessFactor[4]       : SV_TessFactor;
    float InsideTessFactor[2] : SV_InsideTessFactor;
};

// Control point order: [0]=TL, [1]=BL, [2]=TR, [3]=BR
// u increases TL→TR, v increases TL→BL

[RootSignature(ROOTSIG)]
[domain("quad")]
PSInput DSMain(HsPatchOutput constants,
               float2 UV : SV_DomainLocation,
               const OutputPatch<HSInput, 4> patch)
{
    float3 WorldPos = lerp(lerp(patch[0].WorldPos, patch[2].WorldPos, UV.x),
                           lerp(patch[1].WorldPos, patch[3].WorldPos, UV.x), UV.y);
    float2 TexUV    = lerp(lerp(patch[0].TexUV,    patch[2].TexUV,    UV.x),
                           lerp(patch[1].TexUV,    patch[3].TexUV,    UV.x), UV.y);

    float Scale     = Water.RcpTileSize;
    float HalfScale = Scale * 0.5;

    WorldPos.xz += DisplacementMap.SampleLevel(LinearSampler, TexUV, 0) * Scale;
    WorldPos.y  += HeightMap.SampleLevel(LinearSampler, TexUV, 0) * HalfScale;

    float2 Gradient = GradientMap.SampleLevel(LinearSampler, TexUV, 0) * HalfScale;
    float3 Normal   = normalize(float3(-Gradient.x, 1.0, -Gradient.y));
    Normal = mul(float4(Normal, 0.0), Water.WorldMat).xyz;

    PSInput Out;
    Out.PositionH = mul(float4(WorldPos, 1.0), Water.WorldViewProjMat);
    Out.PositionW = WorldPos;
    Out.NormalW   = Normal;
    return Out;
}
