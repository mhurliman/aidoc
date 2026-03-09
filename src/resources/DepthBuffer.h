#pragma once

#include "GpuResource.h"

class DepthBuffer : public GpuResource
{
public:
    void Create(ID3D12Device* device, UINT width, UINT height,
                DXGI_FORMAT format = DXGI_FORMAT_D32_FLOAT);

    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const { return m_dsvHandle; }
    float GetClearDepth() const { return m_clearDepth; }

private:
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
    float m_clearDepth = 1.0f;
};
