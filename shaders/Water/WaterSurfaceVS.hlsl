
#include "WaterCommon.hlsli"

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

Texture2D<float> HeightMap : register(t0);
Texture2D<float2> GradientMap : register(t1);
Texture2D<float2> DisplacementMap : register(t2);

SamplerState LinearSampler : register(s0);

[RootSignature(ROOTSIG)]
PSInput VSMain(VSInput Input)
{
    float Scale = Water.RcpTileSize;
    float HalfScale = Scale * 0.5;

    float3 WaterPosition = Input.Position;

    WaterPosition.xz += DisplacementMap.SampleLevel(LinearSampler, Input.UV, 0) * Scale;
    WaterPosition.y += HeightMap.SampleLevel(LinearSampler, Input.UV, 0) * HalfScale;
    
    float2 Gradient = GradientMap.SampleLevel(LinearSampler, Input.UV, 0) * HalfScale;
    float3 Normal = normalize(float3(-Gradient.x, 1.0, -Gradient.y));
    
    PSInput Out;
    Out.PositionH = mul(float4(WaterPosition, 1.0), Water.WorldViewProjMat);
    Out.PositionW = mul(float4(WaterPosition, 1.0), Water.WorldMat).xyz;
    Out.NormalW   = mul(float4(Normal, 0.0),        Water.WorldMat).xyz;
    return Out;
}