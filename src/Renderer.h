#pragma once

#include "IRenderer.h"
#include "helpers/GraphicsContext.h"
#include "helpers/LinearAllocator.h"
#include "helpers/DescriptorHeap.h"
#include "managers/ResourceManager.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>

class ColorBuffer;
class DepthBuffer;

using Microsoft::WRL::ComPtr;

class Renderer : public IRenderer
{
public:
    void Init(ID3D12Device* device, ID3D12CommandQueue* queue, UINT frameCount);
    void Shutdown();

    // Frame lifecycle — call BeginFrame before RenderScene, EndFrame after.
    void BeginFrame(ID3D12CommandAllocator* allocator, ColorBuffer& rt, DepthBuffer& ds,
                    const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, UINT frameIndex,
                    UINT64 completedFenceValue);
    void RenderScene(const Scene& scene, const View& view,
                     const FrameConstants& frameConstants) override;
    UINT64 EndFrame(ColorBuffer& rt, ID3D12Fence* fence, UINT64& nextFenceValue);

    ResourceManager& GetResourceManager() { return m_resourceManager; }
    TransientDescriptorHeap& GetTransientHeap() { return m_transientHeap; }
    ID3D12GraphicsCommandList* GetCommandList() { return m_gfxContext.GetCommandList(); }

private:
    void CreateRootSignature();
    void InitShaders();

    ID3D12Device* m_device = nullptr;
    GraphicsContext m_gfxContext;
    LinearAllocator m_linearAllocator;
    TransientDescriptorHeap m_transientHeap;
    ResourceManager m_resourceManager;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrvHandle = {};
    UINT m_frameCount = 0;
};
