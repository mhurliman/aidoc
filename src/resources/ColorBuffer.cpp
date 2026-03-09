#include "ColorBuffer.h"
#include "../helpers/Assert.h"

void ColorBuffer::InitFromSwapChain(ID3D12Device* device, IDXGISwapChain* swapChain,
                                    UINT bufferIndex, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle)
{
    ASSERT_SUCCEEDED(swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&m_resource)));
    device->CreateRenderTargetView(m_resource.Get(), nullptr, rtvHandle);
    m_rtvHandle = rtvHandle;
    m_currentState = D3D12_RESOURCE_STATE_PRESENT;
}
