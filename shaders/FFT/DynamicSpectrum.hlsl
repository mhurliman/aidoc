
#include "FFTCommon.hlsli"

ConstantBuffer<FFTParameters> FFT : register(b0);
cbuffer Globals : register(b1)
{
    float SimulationTime;
    float Choppiness;
    // Debug: keep only modes with |n| <= ModeLimit on each axis, zeroing the rest. 0 means no limit.
    //
    // This exists to settle a question the overlay cannot answer on its own. The CPU buoyancy sum is
    // a truncated series and this one is not, so the two disagree by whatever the missing bands carry
    // - and that difference hides anything else that might be wrong between them. Clamp the GPU to
    // the SAME band and the truncation cancels: what is left over is a real defect, and nothing left
    // over means the two implementations agree exactly.
    int ModeLimit;
}

Texture2D<float4> H0 : register(t0);

RWTexture2D<float2> Height : register(u0);
RWTexture2D<float2> Gradient : register(u1);
RWTexture2D<float2> Displacement : register(u2);

[RootSignature(ROOTSIG)]
[NumThreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float DeltaK = 2 * Pi * FFT.RcpTileSize;
    float HalfTexSize = float(FFT.TextureSize) * 0.5;

    float Nx = float(DTid.x) - HalfTexSize;
    float Nz = float(DTid.y) - HalfTexSize;
    float2 k = float2(Nx, Nz) * DeltaK;

    // Band-limit before anything else, so a clamped mode contributes nothing at all rather than a
    // rounded-down something. The DC term goes too: the CPU sum skips (0,0) explicitly.
    bool insideBand = (ModeLimit <= 0)
                   || (abs(Nx) <= (float)ModeLimit && abs(Nz) <= (float)ModeLimit);
    if (!insideBand || (Nx == 0 && Nz == 0))
    {
        Height[DTid.xy]       = float2(0, 0);
        Gradient[DTid.xy]     = float2(0, 0);
        Displacement[DTid.xy] = float2(0, 0);
        return;
    }

    float Theta = SimulationTime * Omega(k);
    float2 E = float2(cos(Theta), sin(Theta));
    
    float4 h0 = H0[DTid.xy];
    float2 h = ComplexMult(h0.xy, E) + ComplexMult(h0.zw, ComplexConj(E));

    float2 ih = float2(-h.y, h.x);
    float2 ikh = ComplexMult(k, ih);

    float2 Offset = Choppiness * ikh / (length(k) + 1e-3);

    Height[DTid.xy] = h;
    Gradient[DTid.xy] = ikh;
    Displacement[DTid.xy] = Offset;
}