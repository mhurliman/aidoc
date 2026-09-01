#include "Common.hlsli"
#include "Shadow.hlsli"

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

struct Light {
    float3 color;
    uint type;
    float3 direction;
    float linearFalloff;
    float3 position;
    float quadraticFalloff;
    float spotAngleCos;
    float3 _pad;
};

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float3 cameraPos;
    int numLights;
};

cbuffer MaterialConstants : register(b1) {
    float4 baseColorFactor;
    float3 emissiveFactor;
    float roughness;
    float metallic;
    int hasTexture;
    int hasEmissiveTexture;
    int twoSided;
};

// Per-object world transform (b2). Defaults to identity for statically-placed meshes;
// driven per-frame for the sim boat. Stored transposed on the CPU (same convention as viewProj).
cbuffer ObjectConstants : register(b2) {
    float4x4 world;
};

Texture2D baseColorTex : register(t0);
Texture2D emissiveTex  : register(t1);
StructuredBuffer<Light> lights : register(t2);
SamplerState linearSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct PSInput {
    float4 clipPos   : SV_POSITION;
    float3 worldPos  : WORLDPOS;
    float3 worldNorm : WORLDNORM;
    float2 uv        : TEXCOORD;
};

[RootSignature(ROOTSIG)]
PSInput VSMain(VSInput input) {
    PSInput output;
    float4 worldPos = mul(float4(input.position, 1.0), world);
    output.clipPos = mul(worldPos, viewProj);
    output.worldPos = worldPos.xyz;
    output.worldNorm = mul(input.normal, (float3x3)world);
    output.uv = input.uv;
    return output;
}

static const float PI = 3.14159265359;

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float denom = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float G_SchlickGGX(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NoV, float NoL, float r) {
    float k = ((r + 1.0) * (r + 1.0)) / 8.0;
    return G_SchlickGGX(NoV, k) * G_SchlickGGX(NoL, k);
}

float3 F_Schlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float Attenuation(Light light, float dist) {
    return 1.0 / (1.0 + light.linearFalloff * dist + light.quadraticFalloff * dist * dist);
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.worldNorm);
    float3 V = normalize(cameraPos - input.worldPos);

    // Two-sided lighting for thin geometry. A sail is a surface with no inside: which face you are
    // looking at is a fact about the camera, not about the cloth, so the normal is turned to face the
    // viewer. Without this a back face is lit by a normal pointing away and comes out black - and
    // worse, the whole sail switches between lit and unlit the moment the boom crosses the
    // centreline and the sim's published leeward direction legitimately changes sides.
    //
    // Gated on the material rather than applied to everything: on closed geometry a grazing-angle
    // front face can have an interpolated normal that points slightly away, and flipping it there
    // would put a dark rim around every silhouette.
    if (twoSided && dot(N, V) < 0.0)
        N = -N;

    // Base color
    float4 baseColor = baseColorFactor;
    if (hasTexture) {
        baseColor *= baseColorTex.Sample(linearSampler, input.uv);
    }

    float3 albedo = baseColor.rgb;
    float alpha = baseColor.a;

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float NoV = max(dot(N, V), 0.001);

    float3 Lo = float3(0, 0, 0);
    float sunVis = ComputeSunShadow(input.worldPos);

    for (int i = 0; i < numLights; i++) {
        Light light = lights[i];

        float3 L;
        float attenuation = 1.0;

        if (light.type == LIGHT_DIRECTIONAL) {
            // light.direction is the travel direction (sun→scene); the surface→light vector is its negation.
            L = normalize(-light.direction);
            attenuation = sunVis;   // only the sun casts shadows
        } else {
            float3 toLight = light.position - input.worldPos;
            float dist = length(toLight);
            L = toLight / dist;
            attenuation = Attenuation(light, dist);

            if (light.type == LIGHT_SPOT) {
                float cosAngle = dot(-L, normalize(light.direction));
                float outerCos = light.spotAngleCos;
                float innerCos = lerp(outerCos, 1.0, 0.1);
                attenuation *= saturate((cosAngle - outerCos) / (innerCos - outerCos));
            }
        }

        float3 H = normalize(V + L);

        float NoH = max(dot(N, H), 0.0);
        float NoL = max(dot(N, L), 0.0);
        float HoV = max(dot(H, V), 0.0);

        float D = D_GGX(NoH, roughness);
        float G = G_Smith(NoV, NoL, roughness);
        float3 F = F_Schlick(HoV, F0);

        float3 specular = (D * G * F) / (4.0 * NoV * NoL + 0.001);

        float3 kD = (1.0 - F) * (1.0 - metallic);
        float3 diffuse = kD * albedo / PI;

        Lo += (diffuse + specular) * light.color * NoL * attenuation;
    }

    float3 ambient = 0.03 * albedo;

    // Emissive
    float3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
        emissive *= emissiveTex.Sample(linearSampler, input.uv).rgb;
    }

    float3 color = ambient + Lo + emissive;

    // Reinhard tone mapping
    color = color / (color + 1.0);
    // Gamma correction
    color = pow(color, 1.0 / 2.2);

    return float4(color, alpha);
}
