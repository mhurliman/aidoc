#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>
#include <vector>
#include "IResource.h"

using Microsoft::WRL::ComPtr;

class Texture;
class GraphicsContext;
class LinearAllocator;
class TransientDescriptorHeap;

enum class ShadingModel
{
    PBR,
    Phong
};

class Material : public IResource
{
public:
    ~Material() override = default;

    // GPU constant buffer layout — must match MaterialConstants in shaders (register b1)
    struct MaterialConstants
    {
        DirectX::XMFLOAT4 baseColorFactor;
        DirectX::XMFLOAT3 emissiveFactor;
        float roughness;
        float metallic;
        int hasTexture;
        int hasEmissiveTexture;
        float _pad;
    };

    // Shared pipeline state that all materials reference.
    // Loads shader blobs from disk and stores the root signature.
    // Call once at startup before creating any materials.
    static void InitSharedState(ID3D12Device* device, ID3D12RootSignature* rootSignature);

    // Create the PSO for this material's specific rasterizer/blend state.
    // Must be called after InitSharedState and after material properties are set.
    void CreatePSO(ID3D12Device* device);

    // Bind this material's PSO, constant buffer, and texture descriptors.
    // Root param 1 = material CBV (b1), root param 2 = texture descriptor table.
    void Bind(GraphicsContext& ctx, LinearAllocator& allocator,
              TransientDescriptorHeap& transientHeap, ID3D12Device* device,
              D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle);

    ID3D12PipelineState* GetPSO() const { return m_pso.Get(); }

    // Public material properties (editable via ImGui)
    std::string name;
    float roughness = 0.5f;
    float metallic = 0.0f;
    DirectX::XMFLOAT4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 emissiveFactor = {0.0f, 0.0f, 0.0f};
    bool doubleSided = false;
    bool alphaBlend = false;
    ShadingModel shadingModel = ShadingModel::PBR;

    std::shared_ptr<Texture> baseColorTexture;
    std::shared_ptr<Texture> emissiveTexture;

    bool HasBaseColorTexture() const { return baseColorTexture != nullptr; }
    bool HasEmissiveTexture() const { return emissiveTexture != nullptr; }

private:
    ComPtr<ID3D12PipelineState> m_pso;

    static std::vector<BYTE> LoadShaderBlob(const std::string& filename);

    // Shared across all materials
    static ID3D12RootSignature* s_rootSignature;
    static std::vector<BYTE> s_pbrVsBlob;
    static std::vector<BYTE> s_pbrPsBlob;
    static std::vector<BYTE> s_phongVsBlob;
    static std::vector<BYTE> s_phongPsBlob;
};
