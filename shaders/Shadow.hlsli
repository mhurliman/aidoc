#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

// Cascaded shadow map sampling for the lit mesh shaders. Registers b3/t3/s1 are appended to the
// mesh root signature (Common.hlsli ROOTSIG). cascadeViewProj are row-vector matrices uploaded
// transposed (same convention as viewProj). A fragment picks the first cascade that contains it.
cbuffer ShadowConstants : register(b3)
{
    float4x4 cascadeViewProj[3];
    float4   shadowParams;   // x = texel size, y = depth bias (NDC), z = cascade count, w = enabled
};

Texture2DArray<float>  ShadowMap     : register(t3);
SamplerComparisonState ShadowSampler : register(s1);

// Sun visibility at a world position: 1 = fully lit, 0 = fully shadowed (PCF 3x3).
float ComputeSunShadow(float3 worldPos)
{
    if (shadowParams.w < 0.5) return 1.0;

    int cascade = -1;
    float3 ndc = float3(0, 0, 0);
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float4 lp = mul(float4(worldPos, 1.0), cascadeViewProj[c]);
        float3 p = lp.xyz / lp.w;
        if (all(abs(p.xy) < 1.0) && p.z >= 0.0 && p.z <= 1.0) { cascade = c; ndc = p; break; }
    }
    if (cascade < 0) return 1.0;   // outside all cascades → treat as lit

    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    float compare = ndc.z - shadowParams.y * (cascade + 1);   // scale bias with cascade size
    float texel = shadowParams.x;

    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += ShadowMap.SampleCmpLevelZero(
                ShadowSampler, float3(uv + float2(x, y) * texel, cascade), compare);
    return sum / 9.0;
}

#endif
