#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "Mesh.h"
#include <stdexcept>

static inline void ThrowIfFailed(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) + " (hr=0x" +
            std::to_string(hr) + ")");
    }
}

void Mesh::LoadFromGltf(const std::string& gltfPath) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    if (!loader.LoadASCIIFromFile(&model, &err, &warn, gltfPath)) {
        throw std::runtime_error("Failed to load glTF: " + err);
    }

    // Use the first primitive of the first mesh
    const auto& mesh = model.meshes[0];
    const auto& primitive = mesh.primitives[0];

    // Read indices
    {
        const auto& accessor = model.accessors[primitive.indices];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];
        const uint8_t* dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

        m_indexCount = static_cast<UINT>(accessor.count);
        m_indices.resize(accessor.count);

        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(dataPtr);
            m_indices.assign(src, src + accessor.count);
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(dataPtr);
            for (size_t i = 0; i < accessor.count; i++) {
                m_indices[i] = src[i];
            }
        }
    }

    // Read positions
    const float* positions = nullptr;
    size_t vertexCount = 0;
    {
        auto it = primitive.attributes.find("POSITION");
        if (it == primitive.attributes.end()) {
            throw std::runtime_error("Mesh has no POSITION attribute");
        }
        const auto& accessor = model.accessors[it->second];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];
        positions = reinterpret_cast<const float*>(
            buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
        vertexCount = accessor.count;
    }

    // Read normals
    const float* normals = nullptr;
    {
        auto it = primitive.attributes.find("NORMAL");
        if (it == primitive.attributes.end()) {
            throw std::runtime_error("Mesh has no NORMAL attribute");
        }
        const auto& accessor = model.accessors[it->second];
        const auto& bufferView = model.bufferViews[accessor.bufferView];
        const auto& buffer = model.buffers[bufferView.buffer];
        normals = reinterpret_cast<const float*>(
            buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
    }

    // Interleave into MeshVertex
    m_vertices.resize(vertexCount);
    for (size_t i = 0; i < vertexCount; i++) {
        m_vertices[i].position[0] = positions[i * 3 + 0];
        m_vertices[i].position[1] = positions[i * 3 + 1];
        m_vertices[i].position[2] = positions[i * 3 + 2];
        m_vertices[i].normal[0] = normals[i * 3 + 0];
        m_vertices[i].normal[1] = normals[i * 3 + 1];
        m_vertices[i].normal[2] = normals[i * 3 + 2];
    }
}

void Mesh::CreateGpuResources(ID3D12Device* device) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    // Vertex buffer
    {
        UINT size = static_cast<UINT>(m_vertices.size() * sizeof(MeshVertex));
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_vertexBuffer)),
            "Failed to create mesh vertex buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped),
            "Failed to map mesh vertex buffer");
        memcpy(mapped, m_vertices.data(), size);
        m_vertexBuffer->Unmap(0, nullptr);

        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.StrideInBytes = sizeof(MeshVertex);
        m_vertexBufferView.SizeInBytes = size;
    }

    // Index buffer
    {
        UINT size = static_cast<UINT>(m_indices.size() * sizeof(uint32_t));
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_indexBuffer)),
            "Failed to create mesh index buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_indexBuffer->Map(0, &readRange, &mapped),
            "Failed to map mesh index buffer");
        memcpy(mapped, m_indices.data(), size);
        m_indexBuffer->Unmap(0, nullptr);

        m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        m_indexBufferView.SizeInBytes = size;
    }
}
