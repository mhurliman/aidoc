
#include "FFTCommon.hlsli"

Texture2D<float2> InputBuffer : register(t0);
RWTexture2D<float2> OutputBuffer : register(u0);

[RootSignature(ROOTSIG)]
[NumThreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float Step = (DTid.x + DTid.y) & 0x1;
    float Factor = (1.0 - 2.0 * Step);

    OutputBuffer[DTid.xy] = InputBuffer[DTid.xy] * Factor;
}
