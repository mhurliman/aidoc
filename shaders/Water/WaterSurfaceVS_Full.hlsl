
#include "WaterCommon.hlsli"

ConstantBuffer<WaterConstants> Water : register(b1);

Texture2D<float>  HeightMap       : register(t0);
Texture2D<float2> GradientMap     : register(t1);
Texture2D<float2> DisplacementMap : register(t2);

SamplerState LinearSampler : register(s0);

[RootSignature(ROOTSIG)]
PSInput VSMain(VSInput Input)
{
    float3 WorldPos = mul(float4(Input.Position, 1.0), Water.WorldMat).xyz;
    float2 TexUV    = Input.UV;

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
