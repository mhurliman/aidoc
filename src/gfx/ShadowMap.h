#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include "IRenderer.h"   // View

using Microsoft::WRL::ComPtr;

// Cascaded shadow map: one depth Texture2DArray with kCascades slices. Each frame the cascades are
// fit to slices of the camera frustum (stable bounding-sphere ortho), casters are rendered depth-
// only into each slice, then the array is sampled (comparison PCF) by the lit shaders.
class ShadowMap
{
public:
    static constexpr int kCascades = 3;

    void Init(ID3D12Device* device, UINT resolution);

    // Recompute per-cascade light view-proj matrices from the camera view + sun *travel* direction.
    void ComputeCascades(const View& view, const DirectX::XMFLOAT3& lightDir);

    ID3D12Resource*             GetResource()      const { return m_resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV(int cascade) const { return m_dsv[cascade]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetArraySRV()      const { return m_srv; }  // non-shader-visible; copy to a bound heap
    UINT                        GetResolution()    const { return m_resolution; }

    // Row-vector light view-proj per cascade, already transposed for HLSL cbuffer upload.
    const DirectX::XMFLOAT4X4* CascadeViewProjT() const { return m_cascadeVPT; }
    const float*               SplitDepths()      const { return m_splitDepths; }

    // Issue a resource-state transition on the whole array (raw barrier; flush the context first).
    void Barrier(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES to);

private:
    ComPtr<ID3D12Resource>       m_resource;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_dsv[kCascades] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE  m_srv = {};
    D3D12_RESOURCE_STATES        m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    UINT                         m_resolution = 2048;

    DirectX::XMFLOAT4X4 m_cascadeVPT[kCascades] = {};
    float               m_splitDepths[kCascades] = {};
};
