#include "FFTCommon.hlsli"

// Box-downsample one mip of the gradient (slope) map into the next. Run as a chain (mip 1, 2, …)
// so the render pixel shader can trilinear-sample averaged slopes at distance, killing the
// specular/reflection sparkle that un-mipped high-frequency normals produce on the horizon.
Texture2D<float2>   Src : register(t0);   // all-mip SRV of the gradient map (read via .mips[SrcMip])
RWTexture2D<float2> Dst : register(u0);   // UAV of the destination mip

cbuffer DSParams : register(b0)
{
    uint SrcMip;   // source mip level to read from
    uint DstW;     // destination mip dimensions
    uint DstH;
    uint _pad;
}

[RootSignature(DOWNSAMPLE_ROOTSIG)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= DstW || id.y >= DstH) return;

    int2 s = int2(id.xy) * 2;
    float2 a = Src.mips[SrcMip][s];
    float2 b = Src.mips[SrcMip][s + int2(1, 0)];
    float2 c = Src.mips[SrcMip][s + int2(0, 1)];
    float2 d = Src.mips[SrcMip][s + int2(1, 1)];
    Dst[id.xy] = (a + b + c + d) * 0.25;
}
