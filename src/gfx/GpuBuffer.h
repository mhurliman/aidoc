#pragma once

#include "GpuResource.h"

// Wraps a linear (buffer-dimension) GPU resource. Serves as the base for
// VertexBuffer and IndexBuffer, and can represent any raw buffer (structured,
// constant, upload staging, etc.).
class GpuBuffer : public GpuResource
{
public:
    void Create(ID3D12Device* device, UINT64 sizeBytes,
                D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState);

    // Maps the buffer, copies data, unmaps. Only valid for UPLOAD heap buffers.
    void Upload(const void* data, UINT64 sizeBytes);

    UINT64                    GetSize()       const { return m_sizeBytes; }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return m_resource->GetGPUVirtualAddress(); }

protected:
    UINT64 m_sizeBytes = 0;
};

// A vertex buffer in UPLOAD heap, persistently accessible after construction.
class VertexBuffer : public GpuBuffer
{
public:
    void CreateAndUpload(ID3D12Device* device, const void* data,
                         UINT64 sizeBytes, UINT strideBytes);

    const D3D12_VERTEX_BUFFER_VIEW& GetView() const { return m_view; }

private:
    D3D12_VERTEX_BUFFER_VIEW m_view = {};
};

// An index buffer in UPLOAD heap, persistently accessible after construction.
class IndexBuffer : public GpuBuffer
{
public:
    void CreateAndUpload(ID3D12Device* device, const void* data,
                         UINT64 sizeBytes, DXGI_FORMAT format);

    const D3D12_INDEX_BUFFER_VIEW& GetView() const { return m_view; }

private:
    D3D12_INDEX_BUFFER_VIEW m_view = {};
};
