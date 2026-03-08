#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

// CPU-only descriptor heap for persistent storage of SRVs.
// Not shader-visible — descriptors are copied to a TransientDescriptorHeap at render time.
class PersistentDescriptorHeap {
public:
    void Init(ID3D12Device* device, UINT capacity,
              D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Allocate a descriptor slot. Returns the CPU handle and the slot index.
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate(UINT& outIndex);

    // Free a previously allocated slot for reuse.
    void Free(UINT index);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(UINT index) const;
    UINT GetDescriptorSize() const { return m_descriptorSize; }
    UINT GetCapacity() const { return m_capacity; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_capacity = 0;
    UINT m_descriptorSize = 0;
    UINT m_nextFreeSlot = 0;
    std::vector<UINT> m_freeList;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
};

// Shader-visible descriptor heap used as a per-frame staging area.
// Descriptors are copied here from persistent heaps before binding to the pipeline.
class TransientDescriptorHeap {
public:
    void Init(ID3D12Device* device, UINT capacity, UINT reservedSlots = 0);

    // Reset the allocator for a new frame. Reserved slots are preserved.
    void Reset();

    // Copy `count` CPU descriptors into the transient heap.
    // Returns the GPU handle to the first copied descriptor.
    D3D12_GPU_DESCRIPTOR_HANDLE CopyDescriptors(
        ID3D12Device* device,
        const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles, UINT count);

    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(UINT index) const;
    UINT GetDescriptorSize() const { return m_descriptorSize; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_capacity = 0;
    UINT m_reservedSlots = 0;
    UINT m_descriptorSize = 0;
    UINT m_currentOffset = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
};
