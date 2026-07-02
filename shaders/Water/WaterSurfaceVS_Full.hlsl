
#include "WaterCommon.hlsli"

ConstantBuffer<WaterConstants> Water : register(b1);

// Geometry displacement only: height (t0) + horizontal displacement (t6).
// The normal is reconstructed per-pixel in the PS from the gradient maps.
Texture2D<float>  HeightMap[NUM_CASCADES]       : register(t0);
Texture2D<float2> DisplacementMap[NUM_CASCADES] : register(t6);

SamplerState LinearSampler : register(s0);

[RootSignature(ROOTSIG)]
PSInput VSMain(VSInput Input)
{
    float3 WorldPos = mul(float4(Input.Position, 1.0), Water.WorldMat).xyz;
    float2 UndispXZ = WorldPos.xz;

    // Sum displacement + height from all cascades, sampled at world UV = worldXZ / tileSize.
    float3 DispHeight = float3(0, 0, 0);
    [unroll] for (int c = 0; c < NUM_CASCADES; ++c)
    {
        float rcp = Water.CascTess[c];
        float2 uv = UndispXZ * rcp;
        DispHeight.xz += DisplacementMap[c].SampleLevel(LinearSampler, uv, 0) * rcp;
        DispHeight.y  += HeightMap[c].SampleLevel(LinearSampler, uv, 0) * (rcp * 0.5);
    }

    WorldPos += DispHeight;

    PSInput Out;
    Out.PositionH = mul(float4(WorldPos, 1.0), Water.WorldViewProjMat);
    Out.PositionW = WorldPos;
    Out.UndispXZ  = UndispXZ;
    return Out;
}
