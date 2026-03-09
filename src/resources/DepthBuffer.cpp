#include "DepthBuffer.h"
#include "../helpers/Assert.h"

void DepthBuffer::Create(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ASSERT_SUCCEEDED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = format;
    clearValue.DepthStencil.Depth = m_clearDepth;

    ASSERT_SUCCEEDED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                                     IID_PPV_ARGS(&m_resource)));

    m_currentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(m_resource.Get(), nullptr, m_dsvHandle);
}
