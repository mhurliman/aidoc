#include "GpuProfiler.h"
#include "util/Assert.h"

void GpuProfiler::Init(ID3D12Device* device, ID3D12CommandQueue* queue, int frameCount)
{
    m_device     = device;
    m_frameCount = frameCount;

    ASSERT_SUCCEEDED(queue->GetTimestampFrequency(&m_ticksPerSecond));

    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kMaxFrames * kMaxScopes * 2;
    ASSERT_SUCCEEDED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap)));

    D3D12_HEAP_PROPERTIES readbackHeap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC   bufDesc      = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = (UINT64)heapDesc.Count * sizeof(UINT64);
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ASSERT_SUCCEEDED(device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuf)));
}

void GpuProfiler::BeginFrame(int frameIndex, ID3D12GraphicsCommandList* cmd)
{
    m_frameIndex = frameIndex;
    m_slotCount  = 0;
    m_stackDepth = 0;

    auto& fd = m_frameData[frameIndex];
    if (fd.valid && fd.slotCount > 0)
    {
        UINT   slotBase   = frameIndex * kMaxScopes * 2;
        UINT64 byteOffset = (UINT64)slotBase * sizeof(UINT64);
        UINT64 readBytes  = (UINT64)fd.slotCount * 2 * sizeof(UINT64);

        D3D12_RANGE readRange = { byteOffset, byteOffset + readBytes };
        void* rawPtr = nullptr;
        m_readbackBuf->Map(0, &readRange, &rawPtr);

        const UINT64* all  = reinterpret_cast<const UINT64*>(rawPtr);
        const UINT64* data = all + slotBase;

        m_results.clear();
        for (int s = 0; s < fd.slotCount; ++s)
        {
            UINT64 t0 = data[s * 2];
            UINT64 t1 = data[s * 2 + 1];
            float  ms = float(t1 - t0) * 1000.0f / float(m_ticksPerSecond);
            m_results.push_back({ fd.names[s], ms });
        }

        D3D12_RANGE noWrite = {};
        m_readbackBuf->Unmap(0, &noWrite);
    }

    fd.slotCount = 0;
    fd.valid     = false;
}

void GpuProfiler::BeginScope(const char* name, ID3D12GraphicsCommandList* cmd)
{
    if (m_slotCount >= kMaxScopes || m_stackDepth >= kMaxScopes)
        return;

    int thisSlot = m_slotCount++;
    m_frameData[m_frameIndex].names[thisSlot] = name;
    m_scopeStack[m_stackDepth++]              = thisSlot;

    UINT query = m_frameIndex * kMaxScopes * 2 + thisSlot * 2;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
}

void GpuProfiler::EndScope(ID3D12GraphicsCommandList* cmd)
{
    if (m_stackDepth == 0)
        return;

    int  thisSlot = m_scopeStack[--m_stackDepth];
    UINT query    = m_frameIndex * kMaxScopes * 2 + thisSlot * 2 + 1;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
}

void GpuProfiler::EndFrame(ID3D12GraphicsCommandList* cmd)
{
    if (m_slotCount == 0)
        return;

    auto& fd      = m_frameData[m_frameIndex];
    fd.slotCount  = m_slotCount;

    UINT slotBase = m_frameIndex * kMaxScopes * 2;
    cmd->ResolveQueryData(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        slotBase,
        m_slotCount * 2,
        m_readbackBuf.Get(),
        (UINT64)slotBase * sizeof(UINT64));

    fd.valid = true;
}
