#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Base class for GPU resources that tracks current D3D12 resource state
// for automatic barrier management by CommandContext.
class GpuResource
{
public:
    virtual ~GpuResource() = default;

    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    ID3D12Resource** GetAddressOf() { return m_resource.GetAddressOf(); }

    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES state) { m_currentState = state; }

protected:
    ComPtr<ID3D12Resource> m_resource;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
};
