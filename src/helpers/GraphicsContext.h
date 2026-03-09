#pragma once

#include "CommandContext.h"

class ColorBuffer;
class DepthBuffer;

// Graphics-specific command context with convenience methods for render target
// management, state setting, and draw calls. Methods that consume GpuResource
// references automatically transition them to the required state before use.
class GraphicsContext : public CommandContext
{
public:
    void ClearRenderTarget(ColorBuffer& target, const float* clearColor);
    void ClearDepth(DepthBuffer& target);
    void SetRenderTarget(ColorBuffer& rt, DepthBuffer& ds);
    void SetViewportAndScissor(const D3D12_VIEWPORT& vp, const D3D12_RECT& scissor);
    void SetRootSignature(ID3D12RootSignature* rootSig);
    void SetPipelineState(ID3D12PipelineState* pso);
    void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void SetVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& vbv);
    void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& ibv);
    void SetConstantBufferView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS cbvAddress);
    void SetShaderResourceView(UINT rootIndex, D3D12_GPU_VIRTUAL_ADDRESS srvAddress);
    void SetDescriptorTable(UINT rootIndex, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    void DrawIndexedInstanced(UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex,
                              UINT startInstance);
};
