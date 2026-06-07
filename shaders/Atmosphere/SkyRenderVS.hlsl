#include "AtmosphereCommon.hlsli"

struct VSOut
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Fullscreen triangle — no vertex buffer needed. Draw 3 vertices, no index buffer.
[RootSignature(ATM_RENDER_ROOTSIG)]
VSOut VSMain(uint vertexId : SV_VertexID)
{
    // NDC positions for a triangle that covers the entire screen.
    float2 ndc = float2(
        (vertexId == 2) ? 3.0 : -1.0,
        (vertexId == 1) ? -3.0 :  1.0
    );

    VSOut Out;
    Out.Position = float4(ndc, 0.0, 1.0);   // z=0 → closest, won't disturb depth
    Out.UV       = ndc * float2(0.5, -0.5) + 0.5;
    return Out;
}
