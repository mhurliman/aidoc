#include "LinearAllocator.h"
#include "Assert.h"
#include <cassert>

void LinearAllocator::Init(ID3D12Device* device, UINT64 perFrameSize, UINT frameCount)
{
    m_perFrameSize = perFrameSize;
    m_frameCount = frameCount;

    UINT64 totalSize = perFrameSize * frameCount;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = totalSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ASSERT_SUCCEEDED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&m_buffer)));

    // Persistently map — upload heaps can stay mapped for the resource's lifetime
    void* mapped = nullptr;
    ASSERT_SUCCEEDED(m_buffer->Map(0, nullptr, &mapped));
    m_mappedPtr = static_cast<uint8_t*>(mapped);
    m_gpuBase = m_buffer->GetGPUVirtualAddress();
}

void LinearAllocator::Shutdown()
{
    if (m_buffer && m_mappedPtr)
    {
        m_buffer->Unmap(0, nullptr);
        m_mappedPtr = nullptr;
    }
    m_buffer.Reset();
}

void LinearAllocator::SetCurrentFrame(UINT frameIndex)
{
    m_currentFrame = frameIndex;
    m_currentOffset = 0;
}

LinearAllocator::Allocation LinearAllocator::Allocate(UINT64 sizeBytes, UINT64 alignment)
{
    // Align the current offset
    UINT64 alignedOffset = (m_currentOffset + alignment - 1) & ~(alignment - 1);

    assert(alignedOffset + sizeBytes <= m_perFrameSize &&
           "LinearAllocator: out of per-frame memory");

    UINT64 globalOffset = static_cast<UINT64>(m_currentFrame) * m_perFrameSize + alignedOffset;

    Allocation alloc;
    alloc.cpuAddress = m_mappedPtr + globalOffset;
    alloc.gpuAddress = m_gpuBase + globalOffset;

    m_currentOffset = alignedOffset + sizeBytes;
    return alloc;
}
