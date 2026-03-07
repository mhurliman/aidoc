#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

struct MeshVertex {
    float position[3];
    float normal[3];
};

class Mesh {
public:
    void LoadFromGltf(const std::string& gltfPath);
    void CreateGpuResources(ID3D12Device* device);

    D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const { return m_vertexBufferView; }
    D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const { return m_indexBufferView; }
    UINT GetIndexCount() const { return m_indexCount; }

private:
    std::vector<MeshVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    UINT m_indexCount = 0;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
};
