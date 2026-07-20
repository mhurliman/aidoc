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
#include <memory>
#include "IResource.h"

using Microsoft::WRL::ComPtr;

class Material;
class ResourceManager;

struct PbrVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

struct SubMesh
{
    std::string name;
    UINT indexCount;
    UINT startIndex;
    INT baseVertex;
    int materialIndex;
};

class Mesh : public IResource
{
public:
    // Parse glTF and create child Texture/Material resources via the ResourceManager.
    void LoadFromGltf(const std::string& path, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ResourceManager& resMgr);

    // Build a single-submesh mesh directly from interleaved-free position/normal arrays (3 floats
    // each per vertex) and an index array, using the supplied material. No SCENE_SCALE is applied —
    // positions are used as-is (e.g. the sim hull mesh, already in metres). Used by the boat.
    void CreateFromArrays(const float* positions, const float* normals, uint32_t vertexCount,
                          const uint32_t* indices, uint32_t indexCount,
                          std::shared_ptr<Material> material, ID3D12Device* device);

    D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const { return m_vbv; }
    D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const { return m_ibv; }

    const std::vector<SubMesh>& GetSubMeshes() const { return m_subMeshes; }
    const std::vector<std::shared_ptr<Material>>& GetMaterials() const { return m_materials; }
    std::vector<std::shared_ptr<Material>>& GetMaterials() { return m_materials; }

private:
    void CreateBuffers(ID3D12Device* device);

    std::vector<PbrVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<SubMesh> m_subMeshes;
    std::vector<std::shared_ptr<Material>> m_materials;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW m_ibv = {};
};
