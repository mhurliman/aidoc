#include "ComputeContext.h"
#include "gfx/ColorBuffer.h"
#include "gfx/DepthBuffer.h"

void ComputeContext::ClearBuffer(
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    GpuResource& Resource,
    const UINT clearValues[4]
)
{
    TransitionResource(Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushResourceBarriers();

    m_commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, Resource.GetResource(), clearValues, 0, nullptr);
}

void ComputeContext::ClearBuffer(
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    GpuResource& Resource,
    const float clearValues[4]
)
{
    TransitionResource(Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FlushResourceBarriers();

    m_commandList->ClearUnorderedAccessViewFloat(gpuHandle, cpuHandle, Resource.GetResource(), clearValues, 0, nullptr);
}

void ComputeContext::SetRootSignature(ID3D12RootSignature* rootSig)
{
    m_commandList->SetComputeRootSignature(rootSig);
}

void ComputeContext::SetConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress)
{
    m_commandList->SetComputeRootConstantBufferView(rootIndex, cbvAddress);
}

void ComputeContext::SetShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS srvAddress)
{
    m_commandList->SetComputeRootShaderResourceView(rootIndex, srvAddress);
}

void ComputeContext::SetDescriptorTable(UINT rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    m_commandList->SetComputeRootDescriptorTable(rootIndex, handle);
}

void ComputeContext::Dispatch(UINT GroupCountX, UINT GroupCountY, UINT GroupCountZ)
{
    FlushResourceBarriers();
    m_commandList->Dispatch(GroupCountX, GroupCountY, GroupCountZ);
}
