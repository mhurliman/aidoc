
#include "WaterCommon.hlsli"

ConstantBuffer<WaterConstants> Water : register(b1);

[RootSignature(ROOTSIG)]
HSInput VSMain(VSInput Input)
{
    HSInput Out;
    Out.WorldPos = mul(float4(Input.Position, 1.0), Water.WorldMat).xyz;
    Out.TexUV    = Input.UV;
    return Out;
}
