#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include "IResource.h"
#include "DescriptorHeap.h"

using Microsoft::WRL::ComPtr;

class Texture : public IResource {
public:
    ~Texture() override = default;

    // Create GPU texture from raw RGBA pixel data and allocate a persistent SRV.
    void CreateFromPixels(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                          const uint8_t* pixels, int width, int height,
                          PersistentDescriptorHeap& srvHeap);

    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVHandle() const { return m_srvHandle; }
    UINT GetSRVIndex() const { return m_srvIndex; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12Resource> m_uploadBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvHandle = {};
    UINT m_srvIndex = UINT_MAX;
    int m_width = 0;
    int m_height = 0;
};
