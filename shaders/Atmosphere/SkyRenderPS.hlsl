#include "AtmosphereCommon.hlsli"

ConstantBuffer<SkyRenderConstants> RC      : register(b0);
Texture2D<float4>                  SkyView : register(t0);
Texture2D<float4>                  TransLUT: register(t1);
SamplerState                       Samp    : register(s0);

struct PSIn
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Reconstruct world-space view direction by unprojecting near and far plane points.
// Avoids any camera-position unit mismatch.
float3 ViewDir(float2 uv)
{
    float2 ndc = uv * float2(2.0, -2.0) - float2(1.0, -1.0);

    float4 clipNear  = float4(ndc, 0.0, 1.0);
    float4 worldNear = mul(clipNear, RC.InvViewProj);
    worldNear /= worldNear.w;

    float4 clipFar  = float4(ndc, 1.0, 1.0);
    float4 worldFar = mul(clipFar, RC.InvViewProj);
    worldFar /= worldFar.w;

    return normalize(worldFar.xyz - worldNear.xyz);
}

float3 SunDisk(float3 rayDir)
{
    static const float kSunAngularRadius = 0.00872; // ~0.5 degrees in radians

    float cosAngle = dot(rayDir, RC.SunDirection);
    float theta    = acos(clamp(cosAngle, -1.0, 1.0));

    float disk   = smoothstep(kSunAngularRadius * 1.04, kSunAngularRadius * 0.96, theta);
    float corona = exp(-theta / (kSunAngularRadius * 5.5));

    float3 camUp    = normalize(RC.CameraPositionAtm);
    float  mu       = dot(camUp, RC.SunDirection);
    float  r        = length(RC.CameraPositionAtm);
    float2 transUV  = float2(
        mu * 0.5 + 0.5,
        clamp((r - RC.BottomRadius) / max(RC.TopRadius - RC.BottomRadius, 1e-5), 0.0, 1.0));
    float3 sunTrans = TransLUT.SampleLevel(Samp, transUV, 0).rgb;

    return (disk * 16.0 + float3(1.0, 0.95, 0.86) * corona * 2.0)
         * RC.SunIntensity * sunTrans * 0.01;
}

[RootSignature(ATM_RENDER_ROOTSIG)]
float4 PSMain(PSIn In) : SV_Target
{
    float3 rayDir = ViewDir(In.UV);
    float3 up     = normalize(RC.CameraPositionAtm);

    float2 skyUV  = SkyViewDirToUv(rayDir, up, RC.SunDirection);
    float3 sky    = SkyView.Sample(Samp, skyUV).rgb;

    sky += SunDisk(rayDir);

    // Tonemap + gamma.
    sky = ACESFilm(sky);
    sky = pow(max(sky, 0.0), 1.0 / 2.2);

    return float4(sky, 1.0);
}
