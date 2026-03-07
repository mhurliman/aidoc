cbuffer SceneConstants : register(b0) {
    float4x4 viewProj;
    float3 cameraPos;
    float roughness;
    float3 lightDir;
    float metallic;
    float4 baseColorFactor;
    int hasTexture;
    float3 emissiveFactor;
    int hasEmissiveTexture;
    float3 _pad;
};

Texture2D baseColorTex : register(t0);
Texture2D emissiveTex  : register(t1);
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

PSInput VSMain(VSInput input) {
    PSInput output;
    output.clipPos = mul(float4(input.position, 1.0), viewProj);
    output.worldPos = input.position;
    output.worldNorm = input.normal;
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

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.worldNorm);
    float3 L = normalize(lightDir);
    float3 V = normalize(cameraPos - input.worldPos);
    float3 H = normalize(V + L);

    // Base color
    float4 baseColor = baseColorFactor;
    if (hasTexture) {
        baseColor *= baseColorTex.Sample(linearSampler, input.uv);
    }

    float3 albedo = baseColor.rgb;
    float alpha = baseColor.a;

    // PBR
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float NoH = max(dot(N, H), 0.0);
    float NoV = max(dot(N, V), 0.001);
    float NoL = max(dot(N, L), 0.0);
    float HoV = max(dot(H, V), 0.0);

    // Cook-Torrance specular BRDF
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);
    float3 F = F_Schlick(HoV, F0);

    float3 specular = (D * G * F) / (4.0 * NoV * NoL + 0.001);

    // Energy-conserving diffuse
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    float3 Lo = (diffuse + specular) * NoL;
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
