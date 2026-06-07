#include "GpuBuffer.h"
#include "util/Assert.h"

void GpuBuffer::Create(ID3D12Device* device, UINT64 sizeBytes,
                        D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES hp = { heapType };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = sizeBytes;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ASSERT_SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                     initialState, nullptr,
                                                     IID_PPV_ARGS(&m_resource)));
    m_currentState = initialState;
    m_sizeBytes    = sizeBytes;
}

void GpuBuffer::Upload(const void* data, UINT64 sizeBytes)
{
    void* mapped;
    m_resource->Map(0, nullptr, &mapped);
    memcpy(mapped, data, sizeBytes);
    m_resource->Unmap(0, nullptr);
}

void VertexBuffer::CreateAndUpload(ID3D12Device* device, const void* data,
                                    UINT64 sizeBytes, UINT strideBytes)
{
    Create(device, sizeBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    Upload(data, sizeBytes);

    m_view.BufferLocation = GetGPUAddress();
    m_view.SizeInBytes    = static_cast<UINT>(sizeBytes);
    m_view.StrideInBytes  = strideBytes;
}

void IndexBuffer::CreateAndUpload(ID3D12Device* device, const void* data,
                                   UINT64 sizeBytes, DXGI_FORMAT format)
{
    Create(device, sizeBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    Upload(data, sizeBytes);

    m_view.BufferLocation = GetGPUAddress();
    m_view.SizeInBytes    = static_cast<UINT>(sizeBytes);
    m_view.Format         = format;
}
