
#include "FFTCommon.hlsli"

ConstantBuffer<FFTParameters> FFT : register(b0);

RWTexture2D<float4> PrecomputedData : register(u0);

[RootSignature(ROOTSIG)]
[NumThreads(1, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint b = FFT.TextureSize >> (DTid.x + 1);
    float i = (2 * b * (DTid.y / b) + (DTid.y % b)) % FFT.TextureSize;

    float2 Mult = 2 * Pi * float2(0, -1) / FFT.TextureSize;
    float2 Twiddle = ComplexExp(Mult * (DTid.y / b) * b);

    uint2 StorePos0 = DTid.xy;
    PrecomputedData[StorePos0] = float4(Twiddle.x, Twiddle.y, i, i + b);
    
    uint2 StorePos1 = DTid.xy + uint2(0, FFT.TextureSize * 0.5);
    PrecomputedData[StorePos1] = float4(-Twiddle.x, -Twiddle.y, i, i + b);
}