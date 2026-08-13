
#include "WaterCommon.hlsli"

ConstantBuffer<FrameConstants> Frame : register(b0);
ConstantBuffer<WaterConstants> Water : register(b1);

// Gradient (slope) maps per cascade — reconstruct the per-pixel normal.
// Bound in the shared t0..t8 table (visibility ALL).
Texture2D<float2>   GradientMap[NUM_CASCADES] : register(t3);
Texture2D<float>    SceneDepth     : register(t9);
Texture2D<float4>   SceneColor     : register(t10);
TextureCube<float4> ReflectionMap  : register(t11);
Texture2DArray<float> ShadowMap    : register(t12);

SamplerState           LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

// Cascaded shadow matrices (matches the mesh ShadowConstants layout).
cbuffer ShadowConstants : register(b2)
{
    float4x4 cascadeViewProj[3];
    float4   shadowParams;   // x = texel size, y = depth bias, z = cascade count, w = enabled
};

static const float PI = 3.14159265;

// ---- Tunables (shader-local; promote to a cbuffer if UI control is wanted) ----
static const float kRefractPixels   = 28.0;   // max refraction offset in pixels
static const float kSpecScale        = 6.0e-4; // GGX sun-glint magnitude
static const float kRoughNear        = 0.03;   // wave roughness up close
static const float kRoughFar         = 0.40;   // wave roughness at the horizon (kills sparkle)
static const float kRoughFarDist     = 300.0;  // metres over which roughness ramps to kRoughFar

float LinearizeDepth(float d)
{
    return Frame.NearZ * Frame.FarZ / (Frame.FarZ - d * (Frame.FarZ - Frame.NearZ));
}

// Sun visibility [0,1] at a world position (PCF 3x3). 1 = lit, 0 = shadowed.
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
    if (cascade < 0) return 1.0;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    float compare = ndc.z - shadowParams.y * (cascade + 1);
    float texel = shadowParams.x;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(uv + float2(x, y) * texel, cascade), compare);
    return sum / 9.0;
}

// Project a world point to screen UV via the view-proj (same matrix the DS uses). Returns false
// if behind the camera. Fills ndcZ with the point's NDC depth.
bool WorldToUV(float3 P, out float2 uv, out float ndcZ)
{
    float4 clip = mul(float4(P, 1.0), Water.WorldViewProjMat);
    uv = 0; ndcZ = 0;
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    uv   = ndc.xy * float2(0.5, -0.5) + 0.5;
    ndcZ = ndc.z;
    return true;
}

// Screen-space reflection: march the reflection ray in world space, project into the depth
// buffer, and return the scene colour where it first passes behind on-screen geometry. Sky
// (far depth) and off-screen rays return confidence 0 so the caller falls back to the cubemap.
float3 ScreenSpaceReflection(float3 origin, float3 dir, out float confidence)
{
    confidence = 0.0;
    float3 P = origin;
    float  step = 0.3;                 // metres; grows geometrically to cover ~90 m in 28 steps
    [loop] for (int i = 0; i < 28; ++i)
    {
        float3 prev = P;
        P += dir * step;
        step *= 1.15;

        float2 uv; float rayZ;
        if (!WorldToUV(P, uv, rayZ)) return 0;
        if (any(uv < 0.0) || any(uv > 1.0)) return 0;      // left the screen → cubemap

        float sceneZ = SceneDepth.SampleLevel(LinearSampler, uv, 0);
        if (sceneZ >= 0.9999) continue;                    // sky → let the cubemap handle it

        if (rayZ > sceneZ)                                 // ray passed behind geometry
        {
            float rayLin = LinearizeDepth(rayZ);
            float scnLin = LinearizeDepth(sceneZ);
            if (rayLin - scnLin < step * 3.0 + 0.5)        // plausible surface, not a far gap
            {
                // Binary refine between the last-in-front and first-behind samples.
                float3 a = prev, b = P;
                [unroll] for (int r = 0; r < 5; ++r)
                {
                    float3 m = (a + b) * 0.5;
                    float2 mu; float mz; WorldToUV(m, mu, mz);
                    float sd = SceneDepth.SampleLevel(LinearSampler, mu, 0);
                    if (mz > sd) b = m; else a = m;
                }
                float2 hu; float hz; WorldToUV(b, hu, hz);
                float2 edge = smoothstep(0.0, 0.12, hu) * smoothstep(0.0, 0.12, 1.0 - hu);
                confidence = edge.x * edge.y;
                return SceneColor.SampleLevel(LinearSampler, hu, 0).rgb;
            }
        }
    }
    return 0;
}

[RootSignature(ROOTSIG)]
float4 PSMain(PSInput Input) : SV_Target
{
    float Depth = Input.PositionH.z;

    // Per-pixel normal from the summed cascade gradients.
    float2 Gradient = float2(0, 0);
    [unroll] for (int c = 0; c < NUM_CASCADES; ++c)
    {
        float rcp = Water.CascTess[c];
        Gradient += GradientMap[c].Sample(LinearSampler, Input.UndispXZ * rcp) * (rcp * 0.5);
    }
    float3 Normal = normalize(float3(-Gradient.x, 1.0, -Gradient.y));
    float3 L      = normalize(-Frame.LightDirection);

    float3 ViewDir    = normalize(Input.PositionW - Frame.CameraPosition); // toward surface
    float3 V          = -ViewDir;                                          // toward camera
    float3 ReflectDir = reflect(ViewDir, Normal);

    // ---- Refraction: bend the background sample by the surface slope (Snell-ish) ----
    uint  sw, sh; SceneColor.GetDimensions(sw, sh);
    float2 invRes  = 1.0 / float2(sw, sh);
    float2 baseUV  = (Input.PositionH.xy) * invRes;
    float2 bentUV  = saturate(baseUV + Normal.xz * kRefractPixels * invRes);
    float  BGDepth = SceneDepth.SampleLevel(LinearSampler, bentUV, 0);
    float3 BGColor = SceneColor.SampleLevel(LinearSampler, bentUV, 0).rgb;
    // Reject bends that pull in geometry in FRONT of the water (foreground bleed).
    if (BGDepth < Depth)
    {
        BGDepth = SceneDepth.SampleLevel(LinearSampler, baseUV, 0);
        BGColor = SceneColor.SampleLevel(LinearSampler, baseUV, 0).rgb;
    }

    float WaterLen = max(LinearizeDepth(BGDepth) - LinearizeDepth(Depth), 0.0);
    float3 DiffuseColor = lerp(Water.ColorMax.rgb, BGColor, exp(-WaterLen * 5.0));

    // Schlick Fresnel.
    float Fresnel = 0.02 + 0.98 * pow(1.0 - max(0, dot(V, Normal)), 5.0);

    // Sky-dome ambient (zenith of the atmosphere cubemap) — diffuse skylight on the water.
    float3 SkyAmbient = ReflectionMap.Sample(LinearSampler, float3(0.0, 1.0, 0.0)).rgb;

    // ---- Reflection: sky cubemap, overlaid with screen-space reflections of on-screen geometry ----
    float3 sky = ReflectionMap.Sample(LinearSampler, ReflectDir).rgb;
    // Below-horizon rays hit the (black) lower hemisphere → substitute a deep-water tint lifted by
    // skylight so grazing wave backs read as lit water rather than black voids.
    float3 UnderwaterColor = Water.ColorMax.rgb * 0.5 + SkyAmbient * 0.2;
    sky = lerp(UnderwaterColor, sky, smoothstep(-0.05, 0.15, ReflectDir.y));

    float  ssrConf;
    float3 ssr = ScreenSpaceReflection(Input.PositionW + Normal * 0.05, ReflectDir, ssrConf);
    float3 EnvColor = lerp(sky, ssr, ssrConf);

    // ---- Sun shadow, GGX glitter (roughness grows with distance to kill horizon sparkle) ----
    float Sun  = ComputeSunShadow(Input.PositionW);
    float dist = length(Input.PositionW - Frame.CameraPosition);
    float rough = lerp(kRoughNear, kRoughFar, saturate(dist / kRoughFarDist));
    float a2   = rough * rough; a2 *= a2;
    float3 H   = normalize(L + V);
    float NoH  = saturate(dot(Normal, H));
    float denom = NoH * NoH * (a2 - 1.0) + 1.0;
    float Dggx = a2 / (PI * denom * denom);
    float Specular = min(Dggx * kSpecScale, 4.0) * Sun;

    float  NDotL = max(0.5, dot(Normal, L));
    float  Shade = lerp(0.28, 1.0, Sun);   // shadowed water keeps some ambient
    EnvColor    *= lerp(0.75, 1.0, Sun);

    // Sun diffuse + sky-dome ambient fill (ambient isn't shadowed by the sun occluder).
    float3 Body = DiffuseColor * (NDotL * Shade + SkyAmbient * Water.Extra.x);
    float3 FinalColor = lerp(Body, EnvColor + Specular, Fresnel);

    return float4(FinalColor, 1.0);
}
