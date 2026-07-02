
#ifndef WATERCOMMON_HLSLI
#define WATERCOMMON_HLSLI

#define NUM_CASCADES 3

// Map table:  t0..t8  = [height x3][grad x3][disp x3]  (visible to all stages)
// Scene table: t9 = depth, t10 = color, t11 = reflection cube (pixel only)
#define ROOTSIG "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), "\
    "CBV(b0), "\
    "CBV(b1), "\
    "DescriptorTable(SRV(t0, numDescriptors = 9), visibility = SHADER_VISIBILITY_ALL), "\
    "DescriptorTable(SRV(t9, numDescriptors = 3), visibility = SHADER_VISIBILITY_PIXEL), "\
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
    float2 UndispXZ  : TEXCOORD0;  // undisplaced world XZ; PS reconstructs the normal per-pixel
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

    float4 ColorMax;   // xyz = Color, w = MaxTessellation
    float4 CascTess;   // xyz = 1/tileSize per cascade, w = TessDistance
};

#endif