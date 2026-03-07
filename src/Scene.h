#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

struct PbrVertex {
    float position[3];
    float normal[3];
    float uv[2];
};

struct SubMesh {
    UINT indexCount;
    UINT startIndex;
    INT baseVertex;
    int materialIndex;
};

struct PbrMaterial {
    int baseColorTextureIndex = -1;
    int emissiveTextureIndex = -1;
    float roughness = 0.5f;
    float metallic = 0.0f;
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
    bool doubleSided = false;
    bool alphaBlend = false;
};

class Scene {
public:
    void LoadFromGltf(const std::string& gltfPath);
    void CreateGpuResources(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);

    D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const { return m_vertexBufferView; }
    D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const { return m_indexBufferView; }

    const std::vector<SubMesh>& GetSubMeshes() const { return m_subMeshes; }
    const std::vector<PbrMaterial>& GetMaterials() const { return m_materials; }
    std::vector<PbrMaterial>& GetMaterials() { return m_materials; }
    UINT GetTextureCount() const { return static_cast<UINT>(m_cpuTextures.size()); }

    void CreateTextureSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT textureIndex) const;

private:
    struct CpuTexture {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
    };

    std::vector<PbrVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<SubMesh> m_subMeshes;
    std::vector<PbrMaterial> m_materials;
    std::vector<CpuTexture> m_cpuTextures;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};

    std::vector<ComPtr<ID3D12Resource>> m_gpuTextures;
    std::vector<ComPtr<ID3D12Resource>> m_uploadBuffers;
};
