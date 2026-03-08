#include "DescriptorHeap.h"
#include <string>

static inline void ThrowIfFailed(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) + " (hr=0x" +
            std::to_string(hr) + ")");
    }
}

// --- PersistentDescriptorHeap ---

void PersistentDescriptorHeap::Init(ID3D12Device* device, UINT capacity,
                                     D3D12_DESCRIPTOR_HEAP_TYPE type) {
    m_capacity = capacity;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    m_nextFreeSlot = 0;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = capacity;
    desc.Type = type;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only
    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)),
        "Failed to create persistent descriptor heap");

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE PersistentDescriptorHeap::Allocate(UINT& outIndex) {
    if (!m_freeList.empty()) {
        outIndex = m_freeList.back();
        m_freeList.pop_back();
    } else {
        if (m_nextFreeSlot >= m_capacity) {
            throw std::runtime_error("PersistentDescriptorHeap: out of descriptors");
        }
        outIndex = m_nextFreeSlot++;
    }
    return GetCPUHandle(outIndex);
}

void PersistentDescriptorHeap::Free(UINT index) {
    m_freeList.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE PersistentDescriptorHeap::GetCPUHandle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
    return handle;
}

// --- TransientDescriptorHeap ---

void TransientDescriptorHeap::Init(ID3D12Device* device, UINT capacity, UINT reservedSlots) {
    m_capacity = capacity;
    m_reservedSlots = reservedSlots;
    m_currentOffset = reservedSlots;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = capacity;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)),
        "Failed to create transient descriptor heap");

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
}

void TransientDescriptorHeap::Reset() {
    m_currentOffset = m_reservedSlots;
}

D3D12_GPU_DESCRIPTOR_HANDLE TransientDescriptorHeap::CopyDescriptors(
    ID3D12Device* device,
    const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles, UINT count) {

    if (m_currentOffset + count > m_capacity) {
        throw std::runtime_error("TransientDescriptorHeap: out of descriptors");
    }

    UINT startSlot = m_currentOffset;
    D3D12_CPU_DESCRIPTOR_HANDLE dstHandle = GetCPUHandle(startSlot);

    // Copy each descriptor individually (src handles may not be contiguous)
    for (UINT i = 0; i < count; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = dstHandle;
        dst.ptr += static_cast<SIZE_T>(i) * m_descriptorSize;
        device->CopyDescriptorsSimple(1, dst, srcHandles[i],
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    m_currentOffset += count;
    return GetGPUHandle(startSlot);
}

D3D12_CPU_DESCRIPTOR_HANDLE TransientDescriptorHeap::GetCPUHandle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE TransientDescriptorHeap::GetGPUHandle(UINT index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuStart;
    handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
    return handle;
}
