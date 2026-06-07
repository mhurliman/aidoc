
#define ROOTSIG "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), "\
    "CBV(b0), "\
    "CBV(b1), "\
    "DescriptorTable(SRV(t0, numDescriptors = 3), visibility = SHADER_VISIBILITY_VERTEX), "\
    "DescriptorTable(SRV(t3, numDescriptors = 3), visibility = SHADER_VISIBILITY_PIXEL)), " \
    "DescriptorTable(Sampler(s0))"

struct VSInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 PositionH : SV_Position;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
};

struct FrameConstants
{
    float3 CameraPosition;
    float3 LightDirection;
};

struct WaterConstants
{
    float4x4 WorldMat;
    float4x4 WorldViewProjMat;
    
    float3 Color;
    
    float TileSize;
    float RcpTileSize;
};

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

Texture2D<float> HeightMap : register(t0);
Texture2D<float2> GradientMap : register(t1);
Texture2D<float2> DisplacementMap : register(t2);

Texture2D<float> SceneDepth : register(t3);
Texture2D<float4> SceneColor : register(t4);
TextureCube<float4> ReflectionMap : register(t5);

SamplerState LinearSampler : register(s0);

[RootSignature(ROOTSIG)]
PSInput VSMain(VSInput Input)
{
    float Scale = Water.RcpTileSize;
    float HalfScale = Scale * 0.5;

    float3 WaterPosition = Input.Position;

    WaterPosition.xz += DisplacementMap.SampleLevel(LinearSampler, Input.UV, 0) * Scale;
    WaterPosition.y += HeightMap.SampleLevel(LinearSampler, Input.UV, 0) * HalfScale;
    
    float2 Gradient = GradientMap.SampleLevel(LinearSampler, Input.UV, 0) * HalfScale;
    float3 Normal = normalize(float3(-Gradient.x, 1.0, -Gradient.y));
    
    PSInput Out;
    Out.PositionH = mul(float4(WaterPosition, 1.0), Water.WorldViewProjMat, );
    Out.PositionW = mul(float4(WaterPosition, 1.0), Water.WorldMat).xyz;
    Out.NormalW = mul(float4(Normal, 0.0), Water.WorldMat).xyz;
    return Out;
}

[RootSignature(ROOTSIG)]
float4 PSMain(PSInput Input) : SV_Target
{
    uint2 PixelCoords = uint2(Input.PositionH.xy);
    float Depth = Input.PositionH.z;
    float3 Normal = Input.NormalW;

    float BGDepth = SceneDepth[PixelCoords];
    float3 BGColor = SceneColor[PixelCoords];

    float WaterLen = max(Depth - BGDepth, 0);
    float3 DiffuseColor = lerp(Water.Color, BGColor, exp(-WaterLen * 0.1));

    float3 ViewDir = normalize(Input.PositionW - Frame.CameraPosition);
    float3 RefractDir = refract(ViewDir, Normal, 0.75);
    float3 ReflectDir = reflect(ViewDir, Normal);

    // Schlick's Approximation
    float Fresnel = 0.02 + 0.98 * pow(1.0 - dot(-ViewDir, Normal), 5.0);

    float3 EnvColor = ReflectionMap.Sample(LinearSampler, ReflectDir).rgb;
    float Specular = pow(max(0, dot(reflect(-Frame.LightDirection, Normal), ViewDir)), 720.0) * 210.0;
    
    float NDotL = max(0, dot(Input.NormalW, -Frame.LightDirection));
    float3 FinalColor = lerp(DiffuseColor * NDotL, EnvColor + Specular, Fresnel);

    return float4(FinalColor, 1.0);
}