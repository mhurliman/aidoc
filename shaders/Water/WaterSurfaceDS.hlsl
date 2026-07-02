
#include "WaterCommon.hlsli"

ConstantBuffer<WaterConstants> Water : register(b1);

// Geometry displacement only: height (t0) + horizontal displacement (t6).
// The normal is reconstructed per-pixel in the PS from the gradient maps.
Texture2D<float>  HeightMap[NUM_CASCADES]       : register(t0);
Texture2D<float2> DisplacementMap[NUM_CASCADES] : register(t6);

SamplerState LinearSampler : register(s0);

struct HsPatchOutput
{
    float TessFactor[4]       : SV_TessFactor;
    float InsideTessFactor[2] : SV_InsideTessFactor;
};

// Sum displacement + height from every cascade, sampled at world UV = worldXZ / tileSize.
float3 SampleDisplacement(float2 worldXZ)
{
    float3 dispHeight = float3(0, 0, 0);
    [unroll] for (int c = 0; c < NUM_CASCADES; ++c)
    {
        float rcp = Water.CascTess[c];
        float2 uv = worldXZ * rcp;
        dispHeight.xz += DisplacementMap[c].SampleLevel(LinearSampler, uv, 0) * rcp;
        dispHeight.y  += HeightMap[c].SampleLevel(LinearSampler, uv, 0) * (rcp * 0.5);
    }
    return dispHeight;
}

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

    float2 UndispXZ = WorldPos.xz;         // sampling coordinate for per-pixel normals
    WorldPos += SampleDisplacement(UndispXZ);

    PSInput Out;
    Out.PositionH = mul(float4(WorldPos, 1.0), Water.WorldViewProjMat);
    Out.PositionW = WorldPos;
    Out.UndispXZ  = UndispXZ;
    return Out;
}
