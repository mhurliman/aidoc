#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// Per-frame linear (bump-pointer) allocator backed by a single persistently-mapped
// upload-heap buffer. Used for dynamic constant buffers and other per-frame GPU data.
//
// The buffer is divided into FrameCount regions. Each frame resets its region's offset
// to zero, which is safe because WaitForFrame ensures the GPU has finished with that
// frame's previous data before we overwrite it.
class LinearAllocator
{
public:
    struct Allocation
    {
        void* cpuAddress;
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress;
    };

    void Init(ID3D12Device* device, UINT64 perFrameSize, UINT frameCount);
    void Shutdown();

    // Call at the start of each frame to reset the current frame's region.
    void SetCurrentFrame(UINT frameIndex);

    // Allocate `sizeBytes` from the current frame's region.
    // Alignment defaults to 256 (D3D12 constant buffer alignment requirement).
    Allocation Allocate(UINT64 sizeBytes, UINT64 alignment = 256);

private:
    ComPtr<ID3D12Resource> m_buffer;
    uint8_t* m_mappedPtr = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase = 0;
    UINT64 m_perFrameSize = 0;
    UINT m_frameCount = 0;
    UINT m_currentFrame = 0;
    UINT64 m_currentOffset = 0;
};
