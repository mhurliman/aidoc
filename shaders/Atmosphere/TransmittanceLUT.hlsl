#include "AtmosphereCommon.hlsli"

ConstantBuffer<AtmosphereParameters> Atm   : register(b0);

// b1 unused for this pass but required by the shared root signature.
struct Dummy { float4 _pad; };
ConstantBuffer<Dummy> _Unused : register(b1);

// t0 unused (no LUT input for this pass) — bound as null SRV from C++.
Texture2D<float4>    _NullSRV : register(t0);
RWTexture2D<float4>  Output   : register(u0);

SamplerState LinearClamp : register(s0);

static const int STEPS = 40;

[RootSignature(ATM_COMPUTE_ROOTSIG)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);

    // Linear parameterisation matching SampleTransmittanceLUT's inverse.
    float mu     = lerp(-1.0, 1.0, uv.x);
    float radius = lerp(Atm.BottomRadius, Atm.TopRadius, uv.y);

    float3 rayOrigin = float3(0.0, radius, 0.0);
    float  sinTheta  = sqrt(max(1.0 - mu * mu, 0.0));
    float3 rayDir    = normalize(float3(sinTheta, mu, 0.0));

    float2 atmHit   = RaySphereIntersect(rayOrigin, rayDir, float3(0,0,0), Atm.TopRadius);
    float2 gndHit   = RaySphereIntersect(rayOrigin, rayDir, float3(0,0,0), Atm.BottomRadius);

    // Ray hits ground — zero transmittance.
    if (gndHit.x > 0.0) { Output[id.xy] = float4(0, 0, 0, 1); return; }

    float rayLen = max(0.0, atmHit.y);
    if (rayLen <= 0.0) { Output[id.xy] = float4(1, 1, 1, 1); return; }

    float dt = rayLen / float(STEPS);
    float3 optDepth = 0.0;

    for (int i = 0; i < STEPS; ++i)
    {
        float3 pt  = rayOrigin + rayDir * ((float(i) + 0.5) * dt);
        float  alt = length(pt) - Atm.BottomRadius;

        float rd = RayleighDensity(alt, Atm.RayleighScaleHeight);
        float md = MieDensity(alt, Atm.MieScaleHeight);
        float od = OzoneDensity(alt, Atm.OzoneCenterHeight, Atm.OzoneWidth);

        optDepth += (Atm.RayleighBeta * rd + Atm.MieBetaExt * md + Atm.OzoneBetaAbs * od) * dt;
    }

    Output[id.xy] = float4(exp(-optDepth), 1.0);
}
