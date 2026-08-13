#include "FFTCommon.hlsli"

// Turns an FFT working texture into a small viewable RGBA image for the debug UI.
//   Mode 0 — time-evolved spectrum h(k,t): fftshift (DC centred) + log-magnitude, heat ramp.
//   Mode 1 — spatial heightfield: signed height, blue(-)→white(0)→orange(+).
// Reuses the downsample root sig (CBV b0 + SRV t0 + UAV u0).
cbuffer VizParams : register(b0)
{
    uint  Mode;
    uint  N;        // source/dest dimension (square)
    float Scale;    // magnitude/height gain before the tone map
    uint  _pad;
}

Texture2D<float2>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

float3 HeatRamp(float t)   // black → red → orange → white
{
    t = saturate(t);
    return saturate(float3(t * 3.0, t * 3.0 - 1.0, t * 3.0 - 2.0));
}

float3 SignedRamp(float h) // h in [-1,1]: blue (down) → white → orange (up)
{
    float3 up   = float3(0.95, 0.55, 0.15);
    float3 down = float3(0.15, 0.35, 0.95);
    float3 tint = h >= 0.0 ? up : down;
    return lerp(float3(0.95, 0.95, 0.95), tint, saturate(abs(h)));
}

[RootSignature(DOWNSAMPLE_ROOTSIG)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= N || id.y >= N) return;

    float3 rgb;
    if (Mode == 0)
    {
        // DynamicSpectrum already stores h(k,t) with DC at the texture centre (texel N/2), so
        // sample directly — no fftshift (adding one would split the lobe into the four corners).
        float mag = length(Src[id.xy]);
        float t   = saturate(log(1.0 + mag * Scale) * 0.5);   // Scale = UI gain
        rgb = HeatRamp(t);
    }
    else
    {
        float h = Src[id.xy].x * Scale;
        rgb = SignedRamp(h);
    }
    Dst[id.xy] = float4(rgb, 1.0);
}
