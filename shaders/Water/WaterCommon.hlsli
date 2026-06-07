
#ifndef WATERCOMMON_HLSLI
#define WATERCOMMON_HLSLI

#define ROOTSIG "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), "\
    "CBV(b0), "\
    "CBV(b1), "\
    "DescriptorTable(SRV(t0, numDescriptors = 3), visibility = SHADER_VISIBILITY_ALL), "\
    "DescriptorTable(SRV(t3, numDescriptors = 3), visibility = SHADER_VISIBILITY_PIXEL), "\
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, "\
    "    addressU = TEXTURE_ADDRESS_WRAP, "\
    "    addressV = TEXTURE_ADDRESS_WRAP, "\
    "    addressW = TEXTURE_ADDRESS_WRAP)"

struct VSInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

// VS → HS: undisplaced world position and UV
struct HSInput
{
    float3 WorldPos : POSITION;
    float2 TexUV    : TEXCOORD0;
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
    float  NearZ;
    float3 LightDirection;
    float  FarZ;
};

struct WaterConstants
{
    float4x4 WorldMat;
    float4x4 WorldViewProjMat;
    
    float3 Color;

    float TileSize;
    float RcpTileSize;
    float MaxTessellation;
    float TessDistance;
    float _pad;
};

#endif