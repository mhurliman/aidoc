#include "AtmosphereCommon.hlsli"

ConstantBuffer<SkyRenderConstants> RC  : register(b0);

// FaceIndices[z] maps each dispatch z-group to a D3D12 cubemap face index (0–5).
// Dispatch 1 sets {2,0,0,0} (face +Y, z-count=1).
// Dispatch 2 sets {0,1,4,5} (side faces, z-count=4).
cbuffer CubeDispatch : register(b1)
{
    uint4 FaceIndices;
};

Texture2D<float4>    SkyView           : register(t0);
Texture2D<float4>    TransLUT          : register(t1);
SamplerState         Samp              : register(s0);
RWTexture2DArray<float4> OutCube       : register(u0);

// Map a cubemap face index + [-1,1] UV to a world-space direction.
// D3D12 face order: +X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5.
float3 FaceDir(uint face, float2 uv)  // face: D3D12 cubemap face index 0–5
{
    switch (face)
    {
        case 0:  return float3( 1.0,  uv.y, -uv.x);
        case 1:  return float3(-1.0,  uv.y,  uv.x);
        case 2:  return float3( uv.x,  1.0, -uv.y);
        case 3:  return float3( uv.x, -1.0,  uv.y);
        case 4:  return float3( uv.x,  uv.y,  1.0);
        default: return float3(-uv.x,  uv.y, -1.0);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h, layers;
    OutCube.GetDimensions(w, h, layers);
    if (id.x >= w || id.y >= h) return;

    // Map this dispatch z-group to a D3D12 cubemap face index via the per-dispatch table.
    uint face = FaceIndices[id.z];

    // Pixel centre in [-1, 1]; flip Y for D3D cubemap convention.
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h) * 2.0 - 1.0;
    uv.y = -uv.y;
    float3 rayDir = normalize(FaceDir(face, uv));

    float3 atmPos = RC.CameraPositionAtm;
    float3 up     = normalize(atmPos);

    // Sample the pre-computed SkyViewLUT.
    // All dispatched directions are in the upper hemisphere — no ground check needed.
    float2 skyUV = SkyViewDirToUv(rayDir, up, RC.SunDirection);
    float3 color = SkyView.SampleLevel(Samp, skyUV, 0).rgb;

    // Add sun disk and corona — same maths as SkyRenderPS.hlsl.
    static const float kSunRad = 0.00872;
    float cosTheta = dot(rayDir, RC.SunDirection);
    float theta    = acos(clamp(cosTheta, -1.0, 1.0));
    float disk     = smoothstep(kSunRad * 1.04, kSunRad * 0.96, theta);
    float corona   = exp(-theta / (kSunRad * 5.5));

    float  mu      = dot(up, RC.SunDirection);
    float2 tUV     = float2(
        mu * 0.5 + 0.5,
        clamp((length(atmPos) - RC.BottomRadius)
              / max(RC.TopRadius - RC.BottomRadius, 1e-5), 0.0, 1.0));
    float3 sunTrans = TransLUT.SampleLevel(Samp, tUV, 0).rgb;

    color += (disk * 16.0 + float3(1.0, 0.95, 0.86) * corona * 2.0)
           * RC.SunIntensity * sunTrans * 0.01;

    OutCube[uint3(id.xy, face)] = float4(color, 1.0);
}
