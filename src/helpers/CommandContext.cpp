#include "CommandContext.h"
#include "../resources/GpuResource.h"
#include "Assert.h"
#include <cassert>

void CommandContext::Init(ID3D12Device* device, ID3D12CommandQueue* queue,
                          D3D12_COMMAND_LIST_TYPE type)
{
    m_device = device;
    m_queue = queue;

    // Create command list in closed state — Begin() will reset it
    ComPtr<ID3D12CommandAllocator> dummyAllocator;
    ASSERT_SUCCEEDED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&dummyAllocator)));
    ASSERT_SUCCEEDED(device->CreateCommandList(0, type, dummyAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_commandList)));
    ASSERT_SUCCEEDED(m_commandList->Close());
}

void CommandContext::Begin(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialPSO)
{
    m_numPendingBarriers = 0;
    ASSERT_SUCCEEDED(allocator->Reset());
    ASSERT_SUCCEEDED(m_commandList->Reset(allocator, initialPSO));
}

UINT64 CommandContext::Finish(ID3D12Fence* fence, UINT64& nextFenceValue)
{
    FlushResourceBarriers();
    ASSERT_SUCCEEDED(m_commandList->Close());

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    UINT64 fenceValue = nextFenceValue++;
    ASSERT_SUCCEEDED(m_queue->Signal(fence, fenceValue));
    return fenceValue;
}

void CommandContext::TransitionResource(GpuResource& resource, D3D12_RESOURCE_STATES newState)
{
    D3D12_RESOURCE_STATES oldState = resource.GetCurrentState();
    if (oldState == newState)
    {
        return;
    }

    assert(m_numPendingBarriers < kMaxPendingBarriers);
    auto& barrier = m_pendingBarriers[m_numPendingBarriers++];
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource.GetResource();
    barrier.Transition.StateBefore = oldState;
    barrier.Transition.StateAfter = newState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    resource.SetCurrentState(newState);

    if (m_numPendingBarriers == kMaxPendingBarriers)
    {
        FlushResourceBarriers();
    }
}

void CommandContext::FlushResourceBarriers()
{
    if (m_numPendingBarriers > 0)
    {
        m_commandList->ResourceBarrier(m_numPendingBarriers, m_pendingBarriers);
        m_numPendingBarriers = 0;
    }
}

void CommandContext::SetDescriptorHeap(ID3D12DescriptorHeap* heap)
{
    ID3D12DescriptorHeap* heaps[] = {heap};
    m_commandList->SetDescriptorHeaps(1, heaps);
}
