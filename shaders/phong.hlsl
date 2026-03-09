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
    float _matPad;
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

PSInput VSMain(VSInput input) {
    PSInput output;
    output.clipPos = mul(float4(input.position, 1.0), viewProj);
    output.worldPos = input.position;
    output.worldNorm = input.normal;
    output.uv = input.uv;
    return output;
}

float Attenuation(Light light, float dist) {
    return 1.0 / (1.0 + light.linearFalloff * dist + light.quadraticFalloff * dist * dist);
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.worldNorm);
    float3 V = normalize(cameraPos - input.worldPos);

    // Base color
    float4 baseColor = baseColorFactor;
    if (hasTexture) {
        baseColor *= baseColorTex.Sample(linearSampler, input.uv);
    }

    float3 albedo = baseColor.rgb;
    float alpha = baseColor.a;

    float shininess = max((1.0 - roughness) * 128.0, 1.0);

    float3 totalDiffuse = float3(0, 0, 0);
    float3 totalSpecular = float3(0, 0, 0);

    for (int i = 0; i < numLights; i++) {
        Light light = lights[i];

        float3 L;
        float attenuation = 1.0;

        if (light.type == LIGHT_DIRECTIONAL) {
            L = normalize(light.direction);
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

        float NdotL = max(dot(N, L), 0.0);
        float NdotH = max(dot(N, H), 0.0);

        totalDiffuse += light.color * NdotL * attenuation;
        totalSpecular += light.color * pow(NdotH, shininess) * 0.5 * attenuation;
    }

    float3 ambient = 0.1 * albedo;
    float3 color = ambient + totalDiffuse * albedo + totalSpecular;

    // Emissive
    float3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
        emissive *= emissiveTex.Sample(linearSampler, input.uv).rgb;
    }
    color += emissive;

    // Gamma correction
    color = pow(color, 1.0 / 2.2);

    return float4(color, alpha);
}
