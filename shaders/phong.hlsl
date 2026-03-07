cbuffer SceneConstants : register(b0) {
    float4x4 mvp;
    float4x4 model;
    float3 cameraPos;
    float _pad0;
    float3 lightDir;
    float _pad1;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
};

struct PSInput {
    float4 clipPos   : SV_POSITION;
    float3 worldPos  : WORLDPOS;
    float3 worldNorm : WORLDNORM;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.clipPos = mul(float4(input.position, 1.0), mvp);
    output.worldPos = mul(float4(input.position, 1.0), model).xyz;
    output.worldNorm = normalize(mul(input.normal, (float3x3)model));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.worldNorm);
    float3 L = normalize(lightDir);
    float3 V = normalize(cameraPos - input.worldPos);
    float3 R = reflect(-L, N);

    // Material
    float3 baseColor = float3(0.8, 0.5, 0.3);
    float3 ambient = 0.1 * baseColor;
    float3 diffuse = max(dot(N, L), 0.0) * baseColor;
    float3 specular = pow(max(dot(R, V), 0.0), 32.0) * float3(1, 1, 1) * 0.5;

    return float4(ambient + diffuse + specular, 1.0);
}
