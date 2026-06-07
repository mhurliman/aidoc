#pragma once

#include "GpuResource.h"

// Wraps a 2D GPU texture that may be used as a UAV, SRV, or both.
// Descriptor creation is left to the caller — this type only handles resource
// lifetime and state tracking so CommandContext::TransitionResource works correctly.
class PixelBuffer : public GpuResource
{
public:
    void Create(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState);

    DXGI_FORMAT GetFormat() const { return m_format; }
    UINT        GetWidth()  const { return m_width;  }
    UINT        GetHeight() const { return m_height; }

private:
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    UINT        m_width  = 0;
    UINT        m_height = 0;
};
