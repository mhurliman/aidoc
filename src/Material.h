#pragma once

#include <DirectXMath.h>
#include <memory>
#include <string>
#include "IResource.h"

class Texture;

class Material : public IResource {
public:
    ~Material() override = default;

    std::string name;
    float roughness = 0.5f;
    float metallic = 0.0f;
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
    bool doubleSided = false;
    bool alphaBlend = false;

    std::shared_ptr<Texture> baseColorTexture;
    std::shared_ptr<Texture> emissiveTexture;

    bool HasBaseColorTexture() const { return baseColorTexture != nullptr; }
    bool HasEmissiveTexture() const { return emissiveTexture != nullptr; }
};
