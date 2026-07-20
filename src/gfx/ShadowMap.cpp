#include "ShadowMap.h"
#include "util/Assert.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

void ShadowMap::Init(ID3D12Device* device, UINT resolution)
{
    m_resolution = resolution;

    // Depth Texture2DArray (typeless so it can be both DSV and SRV).
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = kCascades;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    ASSERT_SUCCEEDED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                     IID_PPV_ARGS(&m_resource)));
    m_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    // One DSV per array slice.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = kCascades;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ASSERT_SUCCEEDED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvBase = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int c = 0; c < kCascades; ++c)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = c;
        dsvDesc.Texture2DArray.ArraySize = 1;
        m_dsv[c] = dsvBase;
        m_dsv[c].ptr += static_cast<SIZE_T>(c) * dsvSize;
        device->CreateDepthStencilView(m_resource.Get(), &dsvDesc, m_dsv[c]);
    }

    // Texture2DArray SRV (non-shader-visible heap; the renderer copies it into a bound heap).
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ASSERT_SUCCEEDED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));
    m_srv = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = kCascades;
    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srv);
}

void ShadowMap::Barrier(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES to)
{
    if (m_state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = m_state;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
    m_state = to;
}

void ShadowMap::ComputeCascades(const View& view, const XMFLOAT3& lightDir)
{
    // Actual (row-vector) camera view/proj — the View stores them transposed for HLSL.
    XMMATRIX V = XMMatrixTranspose(XMLoadFloat4x4(&view.viewMatrix));
    XMMATRIX P = XMMatrixTranspose(XMLoadFloat4x4(&view.projMatrix));
    XMMATRIX invVP = XMMatrixInverse(nullptr, XMMatrixMultiply(V, P));

    // Full-frustum world corners: near plane (NDC z=0) then far plane (NDC z=1).
    XMVECTOR corners[8];
    int i = 0;
    for (float z = 0.0f; z <= 1.0f; z += 1.0f)
        for (float y = -1.0f; y <= 1.0f; y += 2.0f)
            for (float x = -1.0f; x <= 1.0f; x += 2.0f)
                corners[i++] = XMVector3TransformCoord(XMVectorSet(x, y, z, 1.0f), invVP);
    // corners[0..3] = near, corners[4..7] = far, matched by index (same x,y).

    // Shadow range: cap the far distance (distant water needs no shadows) and split logarithmically.
    const float nearZ = view.nearZ;
    const float farZ  = std::min(view.farZ, 300.0f);
    float splits[kCascades + 1];
    splits[0] = nearZ;
    for (int c = 1; c <= kCascades; ++c)
    {
        float f = static_cast<float>(c) / kCascades;
        float logd = nearZ * std::pow(farZ / nearZ, f);
        float lind = nearZ + (farZ - nearZ) * f;
        splits[c] = 0.7f * logd + 0.3f * lind;   // practical log/uniform blend
    }

    XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));   // sun travel direction
    XMVECTOR up = (fabsf(XMVectorGetY(L)) > 0.99f) ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    const float range = farZ - nearZ;

    for (int c = 0; c < kCascades; ++c)
    {
        float tNear = (splits[c]     - nearZ) / range;
        float tFar  = (splits[c + 1] - nearZ) / range;

        // Slice corners: view depth is linear along each near→far frustum edge, so lerp works.
        XMVECTOR slice[8];
        XMVECTOR centroid = XMVectorZero();
        for (int k = 0; k < 4; ++k)
        {
            XMVECTOR nC = corners[k];
            XMVECTOR fC = corners[k + 4];
            slice[k]     = XMVectorLerp(nC, fC, tNear);
            slice[k + 4] = XMVectorLerp(nC, fC, tFar);
            centroid = XMVectorAdd(centroid, XMVectorAdd(slice[k], slice[k + 4]));
        }
        centroid = XMVectorScale(centroid, 1.0f / 8.0f);

        // Bounding sphere → rotation-stable ortho size.
        float radius = 0.0f;
        for (int k = 0; k < 8; ++k)
            radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(slice[k], centroid))));
        radius = std::ceil(radius * 16.0f) / 16.0f;   // quantise to reduce size shimmer

        float backoff = radius + 50.0f;   // pull the light back to catch casters above the slice
        XMVECTOR eye = XMVectorSubtract(centroid, XMVectorScale(L, backoff));
        XMMATRIX lightView = XMMatrixLookAtLH(eye, centroid, up);
        XMMATRIX lightProj = XMMatrixOrthographicLH(2.0f * radius, 2.0f * radius, 0.1f, backoff + radius + 50.0f);

        // Texel-snap the centre in light space to reduce translation shimmer.
        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);
        XMVECTOR originNDC = XMVector3TransformCoord(centroid, lightVP);
        float texPerNDC = m_resolution * 0.5f;
        float sx = XMVectorGetX(originNDC) * texPerNDC;
        float sy = XMVectorGetY(originNDC) * texPerNDC;
        float dx = (std::round(sx) - sx) / texPerNDC;
        float dy = (std::round(sy) - sy) / texPerNDC;
        XMMATRIX snap = XMMatrixTranslation(dx, dy, 0.0f);
        lightVP = XMMatrixMultiply(lightVP, snap);

        XMStoreFloat4x4(&m_cascadeVPT[c], XMMatrixTranspose(lightVP));   // transposed for HLSL upload
        m_splitDepths[c] = splits[c + 1];
    }
}
