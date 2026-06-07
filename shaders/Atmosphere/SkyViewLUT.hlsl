#include "AtmosphereCommon.hlsli"

ConstantBuffer<AtmosphereParameters> Atm     : register(b0);
ConstantBuffer<SkyViewConstants>     Sky     : register(b1);
Texture2D<float4>                    TransLUT : register(t0);
RWTexture2D<float4>                  Output  : register(u0);
SamplerState                         LinearClamp : register(s0);

static const int STEPS = 32;

[RootSignature(ATM_COMPUTE_ROOTSIG)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);

    float3 camPos = Sky.CameraPositionAtm;
    float3 up     = normalize(camPos);
    float3 rayDir = SkyViewUvToDir(uv, up, Sky.SunDirection);

    float3 planetCenter = float3(0, 0, 0);
    float2 atmHit = RaySphereIntersect(camPos, rayDir, planetCenter, Atm.TopRadius);
    float2 gndHit = RaySphereIntersect(camPos, rayDir, planetCenter, Atm.BottomRadius);

    if (atmHit.y <= 0.0) { Output[id.xy] = float4(0, 0, 0, 1); return; }

    float atmNear = max(atmHit.x, 0.0);
    float atmFar  = atmHit.y;
    if (gndHit.x > atmNear) atmFar = min(atmFar, gndHit.x);

    float segLen = atmFar - atmNear;
    float dt     = segLen / float(STEPS);

    float  mu     = dot(rayDir, Sky.SunDirection);
    float  phaseR = RayleighPhase(mu);
    float  phaseM = MiePhase(mu, Atm.MieG);

    float viewRayleighOD = 0.0;
    float viewMieOD      = 0.0;
    float viewOzoneOD    = 0.0;
    float3 totalRayleigh = 0.0;
    float3 totalMie      = 0.0;

    for (int i = 0; i < STEPS; ++i)
    {
        float  t  = atmNear + (float(i) + 0.5) * dt;
        float3 pt = camPos + rayDir * t;
        float  alt = length(pt) - Atm.BottomRadius;

        float rd = RayleighDensity(alt, Atm.RayleighScaleHeight);
        float md = MieDensity(alt, Atm.MieScaleHeight);
        float od = OzoneDensity(alt, Atm.OzoneCenterHeight, Atm.OzoneWidth);

        viewRayleighOD += rd * dt;
        viewMieOD      += md * dt;
        viewOzoneOD    += od * dt;

        float3 sunTrans = SampleTransmittanceLUT(TransLUT, LinearClamp, Atm, pt, Sky.SunDirection);

        float3 viewTau    = Atm.RayleighBeta * viewRayleighOD
                          + Atm.MieBetaExt   * viewMieOD
                          + Atm.OzoneBetaAbs * viewOzoneOD;
        float3 viewTrans  = exp(-viewTau);
        float3 sampleTrans = sunTrans * viewTrans;

        totalRayleigh += rd * sampleTrans * dt;
        totalMie      += md * sampleTrans * dt;
    }

    float3 scattered = Atm.SunIntensity * (
        phaseR * Atm.RayleighBeta * totalRayleigh +
        phaseM * Atm.MieBeta      * totalMie
    );

    Output[id.xy] = float4(scattered, 1.0);
}
