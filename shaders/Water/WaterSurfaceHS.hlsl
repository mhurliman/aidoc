
#include "WaterCommon.hlsli"

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

struct HsPatchOutput
{
    float TessFactor[4]       : SV_TessFactor;
    float InsideTessFactor[2] : SV_InsideTessFactor;
};

float EdgeTessFactor(float3 p0, float3 p1)
{
    float dist = length((p0 + p1) * 0.5 - Frame.CameraPosition);
    return clamp(Water.MaxTessellation * (1.0 - dist / Water.TessDistance), 1.0, Water.MaxTessellation);
}

// Control point order: [0]=TL, [1]=BL, [2]=TR, [3]=BR
// Domain UV: u increases TL→TR, v increases TL→BL
HsPatchOutput PatchConstant(InputPatch<HSInput, 4> patch, uint patchId : SV_PrimitiveID)
{
    HsPatchOutput Out;
    Out.TessFactor[0] = EdgeTessFactor(patch[0].WorldPos, patch[1].WorldPos); // u=0 (left,  TL-BL)
    Out.TessFactor[1] = EdgeTessFactor(patch[0].WorldPos, patch[2].WorldPos); // v=0 (top,   TL-TR)
    Out.TessFactor[2] = EdgeTessFactor(patch[2].WorldPos, patch[3].WorldPos); // u=1 (right, TR-BR)
    Out.TessFactor[3] = EdgeTessFactor(patch[1].WorldPos, patch[3].WorldPos); // v=1 (bot,   BL-BR)
    Out.InsideTessFactor[0] = (Out.TessFactor[0] + Out.TessFactor[2]) * 0.5;
    Out.InsideTessFactor[1] = (Out.TessFactor[1] + Out.TessFactor[3]) * 0.5;
    return Out;
}

[RootSignature(ROOTSIG)]
[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstant")]
HSInput HSMain(InputPatch<HSInput, 4> patch,
               uint i       : SV_OutputControlPointID,
               uint patchId : SV_PrimitiveID)
{
    return patch[i];
}
