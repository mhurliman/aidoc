#pragma once

#include "IRenderer.h"
#include "gfx/CommandContext.h"
#include "gfx/GraphicsContext.h"
#include "gfx/GpuProfiler.h"
#include "gfx/LinearAllocator.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/PixelBuffer.h"
#include "assets/ResourceManager.h"
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
    void RenderScene(Scene& scene, const View& view,
                     const FrameConstants& frameConstants, float elapsedTime) override;
    UINT64 EndFrame(ColorBuffer& rt, ID3D12Fence* fence, UINT64& nextFenceValue);

    // Drop size-dependent targets so they are recreated at the new size on the next BeginFrame.
    // Caller must ensure the GPU is idle first.
    void OnResize() { m_sceneTargetsInitialized = false; }

    ResourceManager& GetResourceManager() { return m_resourceManager; }
    TransientDescriptorHeap& GetTransientHeap() { return m_transientHeap; }
    ID3D12GraphicsCommandList* GetCommandList() { return m_gfxContext.GetCommandList(); }
    GpuProfiler& GetProfiler() { return m_profiler; }

private:
    void InitShaders();

    ID3D12Device*       m_device        = nullptr;
    ID3D12CommandQueue* m_graphicsQueue = nullptr;
    UINT                m_frameCount    = 0;
    UINT                m_frameIndex    = 0;

    GraphicsContext m_gfxContext;
    CommandContext  m_computeContext;
    GpuProfiler     m_profiler;

    ComPtr<ID3D12CommandQueue>                        m_computeQueue;
    std::vector<ComPtr<ID3D12CommandAllocator>>       m_computeAllocators;
    ComPtr<ID3D12Fence>                               m_computeFence;
    UINT64                                            m_nextComputeFenceValue = 1;
    LinearAllocator m_linearAllocator;
    TransientDescriptorHeap m_transientHeap;
    ResourceManager m_resourceManager;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrvHandle = {};

    ColorBuffer* m_currentRT = nullptr;
    DepthBuffer* m_currentDS = nullptr;

    PixelBuffer m_sceneColorCopy;
    ComPtr<ID3D12DescriptorHeap> m_sceneColorSRVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_sceneColorSRVHandle = {};
    bool m_sceneTargetsInitialized = false;
};
