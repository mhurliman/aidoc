#pragma once

#include "GpuResource.h"
#include <dxgi1_4.h>

class ColorBuffer : public GpuResource
{
public:
    // Initialize from an existing swap chain back buffer.
    void InitFromSwapChain(ID3D12Device* device, IDXGISwapChain* swapChain, UINT bufferIndex,
                           D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const { return m_rtvHandle; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
};
