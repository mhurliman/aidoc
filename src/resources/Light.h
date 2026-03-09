#pragma once

#include <DirectXMath.h>
#include <cstdint>

enum class LightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct DirectionalLight
{
    DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
};

struct PointLight
{
    DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    float linearFalloff = 0.09f;
    float quadraticFalloff = 0.032f;
};

struct SpotLight
{
    DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
    float linearFalloff = 0.09f;
    float quadraticFalloff = 0.032f;
    float angle = 0.7854f; // half-angle in radians (~45 degrees)
};

// GPU-side light struct — must match Light in HLSL.
// 64 bytes, 16-byte aligned.
struct GpuLight
{
    DirectX::XMFLOAT3 color;
    uint32_t type;
    DirectX::XMFLOAT3 direction;
    float linearFalloff;
    DirectX::XMFLOAT3 position;
    float quadraticFalloff;
    float spotAngleCos;
    DirectX::XMFLOAT3 _pad;
};

inline GpuLight ToGpuLight(const DirectionalLight& light)
{
    GpuLight gpu = {};
    gpu.color = light.color;
    gpu.type = static_cast<uint32_t>(LightType::Directional);
    gpu.direction = light.direction;
    return gpu;
}

inline GpuLight ToGpuLight(const PointLight& light)
{
    GpuLight gpu = {};
    gpu.color = light.color;
    gpu.type = static_cast<uint32_t>(LightType::Point);
    gpu.position = light.position;
    gpu.linearFalloff = light.linearFalloff;
    gpu.quadraticFalloff = light.quadraticFalloff;
    return gpu;
}

inline GpuLight ToGpuLight(const SpotLight& light)
{
    GpuLight gpu = {};
    gpu.color = light.color;
    gpu.type = static_cast<uint32_t>(LightType::Spot);
    gpu.direction = light.direction;
    gpu.position = light.position;
    gpu.linearFalloff = light.linearFalloff;
    gpu.quadraticFalloff = light.quadraticFalloff;
    gpu.spotAngleCos = cosf(light.angle);
    return gpu;
}
