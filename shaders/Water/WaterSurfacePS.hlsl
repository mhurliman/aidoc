
#include "WaterCommon.hlsli"

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

Texture2D<float>    SceneDepth     : register(t3);
Texture2D<float4>   SceneColor     : register(t4);
TextureCube<float4> ReflectionMap  : register(t5);

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
    float3 Normal      = normalize(Input.NormalW);
    float3 L           = normalize(-Frame.LightDirection);

    float  BGDepth  = SceneDepth[PixelCoords];
    float3 BGColor  = SceneColor[PixelCoords].rgb;

    // Linearize to metric depth before differencing; NDC subtraction is meaningless.
    float WaterLen = max(LinearizeDepth(BGDepth) - LinearizeDepth(Depth), 0.0);
    float3 DiffuseColor = lerp(Water.Color, BGColor, exp(-WaterLen * 5.0));

    float3 ViewDir    = normalize(Input.PositionW - Frame.CameraPosition); // incident (toward surface)
    float3 V          = -ViewDir;                                           // toward camera
    float3 ReflectDir = reflect(ViewDir, Normal);

    // Schlick Fresnel
    float Fresnel = 0.02 + 0.98 * pow(1.0 - max(0, dot(V, Normal)), 5.0);

    float3 EnvColor = ReflectionMap.Sample(LinearSampler, ReflectDir).rgb;
    float  Specular = pow(max(0, dot(reflect(-L, Normal), V)), 720.0) * 210.0;

    float  NDotL      = max(0.5, dot(Normal, L));
    float3 FinalColor = lerp(DiffuseColor * NDotL, EnvColor + Specular, Fresnel);

    return float4(FinalColor, 1.0);
}
