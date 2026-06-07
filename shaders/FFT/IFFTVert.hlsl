
#include "FFTCommon.hlsli"

cbuffer Globals : register(b0)
{
    uint Step;
}

Texture2D<float4> PrecomputedData : register(t0);
Texture2D<float2> InputBuffer : register(t1);

RWTexture2D<float2> OutputBuffer : register(u0);

[RootSignature(ROOTSIG)]
[NumThreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadId)
{
    uint2 LoadPos = uint2(Step, DTid.y);
    float4 Data = PrecomputedData[LoadPos];

    int2 InputIndices = (int2)Data.ba;
    float2 Input0 = InputBuffer[uint2(DTid.x, InputIndices.x)];
    float2 Input1 = InputBuffer[uint2(DTid.x, InputIndices.y)];

    OutputBuffer[DTid.xy] = Input0 + ComplexMult(float2(Data.r, -Data.g), Input1);
}
