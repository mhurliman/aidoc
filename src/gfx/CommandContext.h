#pragma once

class GpuResource;

// Base command context that wraps a command list with automatic barrier batching
// and submit logic. Modeled after MiniEngine's CommandContext.
class CommandContext
{
public:
    virtual ~CommandContext() = default;

    void Init(ID3D12Device* device, ID3D12CommandQueue* queue,
              D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Begin recording. Resets the provided allocator and the command list.
    void Begin(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialPSO = nullptr);

    // Flush pending barriers, close the command list, execute on the queue,
    // and signal the fence. Returns the signaled fence value.
    UINT64 Finish(ID3D12Fence* fence, UINT64& nextFenceValue);

    // Queue a resource transition barrier (batched, flushed lazily).
    void TransitionResource(GpuResource& resource, D3D12_RESOURCE_STATES newState);

    // Issue a UAV barrier to enforce ordering between two dispatches that
    // access the same UAV. Flushes pending transition barriers first.
    void UAVBarrier(GpuResource& resource);

    // Flush all pending barriers immediately.
    void FlushResourceBarriers();

    void SetDescriptorHeap(ID3D12DescriptorHeap* heap);

    // SetPipelineState is valid on both compute and direct command lists.
    void SetPipelineState(ID3D12PipelineState* pso);

    ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }

protected:
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12Device* m_device = nullptr;

    static const UINT kMaxPendingBarriers = 16;
    D3D12_RESOURCE_BARRIER m_pendingBarriers[kMaxPendingBarriers] = {};
    UINT m_numPendingBarriers = 0;
};
