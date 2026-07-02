
#include "WaterCommon.hlsli"

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

// Gradient (slope) maps, one per cascade — used to reconstruct the surface normal per-pixel.
Texture2D<float2>   GradientMap[NUM_CASCADES] : register(t3);
Texture2D<float>    SceneDepth     : register(t9);
Texture2D<float4>   SceneColor     : register(t10);
TextureCube<float4> ReflectionMap  : register(t11);

SamplerState LinearSampler : register(s0);

// Converts an NDC depth sample to linear view-space distance (metres).
// Valid for a standard left-handed projection (near -> 0, far -> 1).
float LinearizeDepth(float d)
{
    return Frame.NearZ * Frame.FarZ / (Frame.FarZ - d * (Frame.FarZ - Frame.NearZ));
}

[RootSignature(ROOTSIG)]
float4 PSMain(PSInput Input) : SV_Target
{
    uint2  PixelCoords = uint2(Input.PositionH.xy);
    float  Depth       = Input.PositionH.z;

    // Reconstruct the normal per-pixel from the summed cascade gradients. Sampling the
    // slope maps here (rather than interpolating a vertex normal) avoids the triangle-edge
    // specular faceting that a razor-sharp highlight otherwise exposes.
    float2 Gradient = float2(0, 0);
    [unroll] for (int c = 0; c < NUM_CASCADES; ++c)
    {
        float rcp = Water.CascTess[c];
        Gradient += GradientMap[c].Sample(LinearSampler, Input.UndispXZ * rcp) * (rcp * 0.5);
    }
    float3 Normal      = normalize(float3(-Gradient.x, 1.0, -Gradient.y));
    float3 L           = normalize(-Frame.LightDirection);

    float  BGDepth  = SceneDepth[PixelCoords];
    float3 BGColor  = SceneColor[PixelCoords].rgb;

    // Linearize to metric depth before differencing; NDC subtraction is meaningless.
    float WaterLen = max(LinearizeDepth(BGDepth) - LinearizeDepth(Depth), 0.0);
    float3 DiffuseColor = lerp(Water.ColorMax.rgb, BGColor, exp(-WaterLen * 5.0));

    float3 ViewDir    = normalize(Input.PositionW - Frame.CameraPosition); // incident (toward surface)
    float3 V          = -ViewDir;                                           // toward camera
    float3 ReflectDir = reflect(ViewDir, Normal);

    // Schlick Fresnel
    float Fresnel = 0.02 + 0.98 * pow(1.0 - max(0, dot(V, Normal)), 5.0);

    float3 EnvColor = ReflectionMap.Sample(LinearSampler, ReflectDir).rgb;

    // The atmosphere cubemap only holds the upper hemisphere (lower half is black).
    // Reflection rays pointing below the horizon — common on the back faces of waves —
    // would sample that black region. Substitute a deep-water tint (the attenuated colour
    // of light within the water body) so back faces read as water rather than voids.
    float3 UnderwaterColor = Water.ColorMax.rgb * 0.6;
    EnvColor = lerp(UnderwaterColor, EnvColor, smoothstep(-0.05, 0.15, ReflectDir.y));

    float  Specular = pow(max(0, dot(reflect(-L, Normal), V)), 720.0) * 210.0;

    float  NDotL      = max(0.5, dot(Normal, L));
    float3 FinalColor = lerp(DiffuseColor * NDotL, EnvColor + Specular, Fresnel);

    return float4(FinalColor, 1.0);
}
