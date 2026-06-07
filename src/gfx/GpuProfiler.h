#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

// Flat + one-level-nested GPU timestamp profiler.
// Layout per frame slot: [scope0_begin, scope0_end, scope1_begin, scope1_end, ...]
// "Total Frame" is always opened first in BeginFrame so it wraps all child scopes.
// Scopes may nest (stack semantics); each BeginScope allocates a slot, EndScope pops it.
class GpuProfiler
{
public:
    static constexpr int kMaxScopes = 16;
    static constexpr int kMaxFrames = 3;

    struct Result { const char* name; float ms; };

    void Init(ID3D12Device* device, ID3D12CommandQueue* queue, int frameCount);

    // Called at the start of a frame after WaitForFrame ensures the GPU slot is free.
    // Reads back results from the previous cycle for this frame index.
    void BeginFrame(int frameIndex, ID3D12GraphicsCommandList* cmd);

    // Called at the end of a frame, before submitting the command list.
    // Issues ResolveQueryData for all scopes recorded this frame.
    void EndFrame(ID3D12GraphicsCommandList* cmd);

    // Allocates a timestamp slot and records a begin timestamp.
    void BeginScope(const char* name, ID3D12GraphicsCommandList* cmd);

    // Records the end timestamp for the most recently opened (stack-top) scope.
    void EndScope(ID3D12GraphicsCommandList* cmd);

    const std::vector<Result>& GetResults() const { return m_results; }

private:
    ID3D12Device* m_device         = nullptr;
    UINT64        m_ticksPerSecond = 1;
    int           m_frameCount     = 0;
    int           m_frameIndex     = 0;
    int           m_slotCount      = 0;  // scopes allocated this frame
    int           m_stackDepth     = 0;
    int           m_scopeStack[kMaxScopes] = {};

    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource>  m_readbackBuf;

    struct FrameData
    {
        int         slotCount              = 0;
        const char* names[kMaxScopes]      = {};
        bool        valid                  = false;
    };
    FrameData m_frameData[kMaxFrames];

    std::vector<Result> m_results;
};
