#pragma once

#include "CommandContext.h"

class ColorBuffer;
class DepthBuffer;

// Graphics-specific command context with convenience methods for render target
// management, state setting, and draw calls. Methods that consume GpuResource
// references automatically transition them to the required state before use.
class ComputeContext : public CommandContext
{
public:
    void ClearBuffer(
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle, 
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
        GpuResource& Resource,
        const UINT clearValues[4]);
        
    void ClearBuffer(
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle, 
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, 
        GpuResource& Resource,
        const float clearValues[4]);

    void SetRootSignature(ID3D12RootSignature* rootSig);

    void SetConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress);
    void SetShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS srvAddress);
    void SetDescriptorTable(UINT rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    
    void Dispatch(UINT groupCountX, UINT groupCountY, UINT groupCountZ);
};
