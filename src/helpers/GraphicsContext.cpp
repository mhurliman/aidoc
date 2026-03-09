#include "GraphicsContext.h"
#include "../resources/ColorBuffer.h"
#include "../resources/DepthBuffer.h"

void GraphicsContext::ClearRenderTarget(ColorBuffer& target, const float* clearColor)
{
    TransitionResource(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    FlushResourceBarriers();
    m_commandList->ClearRenderTargetView(target.GetRTV(), clearColor, 0, nullptr);
}

void GraphicsContext::ClearDepth(DepthBuffer& target)
{
    TransitionResource(target, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    FlushResourceBarriers();
    m_commandList->ClearDepthStencilView(target.GetDSV(), D3D12_CLEAR_FLAG_DEPTH,
                                         target.GetClearDepth(), 0, 0, nullptr);
}

void GraphicsContext::SetRenderTarget(ColorBuffer& rt, DepthBuffer& ds)
{
    TransitionResource(rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    TransitionResource(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    FlushResourceBarriers();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rt.GetRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = ds.GetDSV();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
}

void GraphicsContext::SetViewportAndScissor(const D3D12_VIEWPORT& vp, const D3D12_RECT& scissor)
{
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &scissor);
}

void GraphicsContext::SetRootSignature(ID3D12RootSignature* rootSig)
{
    m_commandList->SetGraphicsRootSignature(rootSig);
}

void GraphicsContext::SetPipelineState(ID3D12PipelineState* pso)
{
    m_commandList->SetPipelineState(pso);
}

void GraphicsContext::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
    m_commandList->IASetPrimitiveTopology(topology);
}

void GraphicsContext::SetVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& vbv)
{
    m_commandList->IASetVertexBuffers(0, 1, &vbv);
}

void GraphicsContext::SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& ibv)
{
    m_commandList->IASetIndexBuffer(&ibv);
}

void GraphicsContext::SetConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress)
{
    m_commandList->SetGraphicsRootConstantBufferView(rootIndex, cbvAddress);
}

void GraphicsContext::SetShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS srvAddress)
{
    m_commandList->SetGraphicsRootShaderResourceView(rootIndex, srvAddress);
}

void GraphicsContext::SetDescriptorTable(UINT rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    m_commandList->SetGraphicsRootDescriptorTable(rootIndex, handle);
}

void GraphicsContext::DrawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndex,
                                           INT baseVertex, UINT startInstance)
{
    FlushResourceBarriers();
    m_commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex,
                                        startInstance);
}
