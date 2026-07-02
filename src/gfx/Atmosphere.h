#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include "gfx/PixelBuffer.h"
#include "gfx/CommandContext.h"
#include "gfx/GraphicsContext.h"
#include "gfx/LinearAllocator.h"
#include "IRenderer.h"

using Microsoft::WRL::ComPtr;

class Atmosphere
{
public:
    // All distances in kilometres. Coefficients in km^-1.
    struct Params
    {
        DirectX::XMFLOAT3 rayleighBeta        = { 0.0058f, 0.0135f, 0.0331f };
        float             rayleighScaleHeight  = 8.0f;

        DirectX::XMFLOAT3 mieBeta             = { 0.021f,  0.021f,  0.021f  };
        float             mieScaleHeight       = 1.2f;

        DirectX::XMFLOAT3 mieBetaExt          = { 0.0231f, 0.0231f, 0.0231f };
        float             mieG                 = 0.758f;

        DirectX::XMFLOAT3 ozoneBetaAbs        = { 0.00065f, 0.00188f, 0.000085f };
        float             ozoneCenterHeight    = 25.0f;

        float             ozoneWidth           = 15.0f;
        float             bottomRadius         = 6371.0f;
        float             topRadius            = 6471.0f;
        float             sunIntensity         = 20.0f;
    };

    Params params;
    bool   visible = true;

    void Init(ID3D12Device* device);

    // Call once per frame before Render().
    // sunDir: normalised direction toward the sun (world space, Y-up).
    // cameraAltitudeM: camera altitude above sea level in metres.
    void Update(CommandContext& ctx, LinearAllocator& alloc,
                const DirectX::XMFLOAT3& sunDir, float cameraAltitudeM);

    // Draws a fullscreen sky background. Call before opaque geometry.
    // Depth write is disabled so opaque geometry renders on top normally.
    void Render(GraphicsContext& ctx, LinearAllocator& alloc, const View& view);

    // Force the transmittance LUT to recompute on the next Update() call.
    void MarkDirty() { m_transmittanceDirty = true; }

    // Demote shared resources from PSR|NPSR back to NPSR on the graphics command list.
    // Must be called at the end of every graphics frame so the next frame's async compute
    // queue can barrier them without encountering PIXEL_SHADER_RESOURCE states.
    void PrepareForCompute(CommandContext& ctx);

    // Returns the atmosphere env cubemap resource (128×128×6, RGBA16F).
    // Valid after the first Update() call; in PIXEL_SHADER_RESOURCE state after Update().
    ID3D12Resource* GetEnvCubeResource() const { return m_envCubeMap.Get(); }

private:
    // Mirrors the HLSL AtmosphereParameters struct (80 bytes).
    struct GpuAtmParams
    {
        DirectX::XMFLOAT3 RayleighBeta;        float RayleighScaleHeight;
        DirectX::XMFLOAT3 MieBeta;             float MieScaleHeight;
        DirectX::XMFLOAT3 MieBetaExt;          float MieG;
        DirectX::XMFLOAT3 OzoneBetaAbs;        float OzoneCenterHeight;
        float OzoneWidth;  float BottomRadius;  float TopRadius;  float SunIntensity;
    };

    struct GpuSkyViewConstants
    {
        DirectX::XMFLOAT3 CameraPositionAtm;   float _pad0;
        DirectX::XMFLOAT3 SunDirection;         float _pad1;
    };

    struct GpuSkyRenderConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj;
        DirectX::XMFLOAT3   CameraPositionAtm;  float _pad0;
        DirectX::XMFLOAT3   SunDirection;        float SunIntensity;
        float BottomRadius;  float TopRadius;     float _pad1[2];
    };

    void CreatePSOs();
    void CreateTextures();
    void CreateDescriptorHeap();
    void BuildDescriptors();

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT slot) const;

    GpuAtmParams BuildGpuParams() const;

    ID3D12Device* m_device = nullptr;

    ComPtr<ID3D12RootSignature> m_computeRootSig;
    ComPtr<ID3D12RootSignature> m_renderRootSig;
    ComPtr<ID3D12RootSignature> m_envCubeRootSig;

    ComPtr<ID3D12PipelineState> m_transmittancePSO;
    ComPtr<ID3D12PipelineState> m_skyViewPSO;
    ComPtr<ID3D12PipelineState> m_skyRenderPSO;
    ComPtr<ID3D12PipelineState> m_envCubePSO;

    PixelBuffer m_transmittanceLUT;   // 256 × 64,  RGBA16F
    PixelBuffer m_skyViewLUT;         // 512 × 256, RGBA16F
    ComPtr<ID3D12Resource> m_envCubeMap;  // 128 × 128 × 6, RGBA16F (TextureCube)
    D3D12_RESOURCE_STATES  m_envCubeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT                         m_descriptorSize = 0;

    bool             m_transmittanceDirty = true;
    DirectX::XMFLOAT3 m_lastSunDir        = { 0.0f, 1.0f, 0.0f };

    static constexpr UINT kEnvCubeSize = 32;  // per-face resolution; sky gradients need very little

    enum : UINT
    {
        kNullSRV          = 0,
        kTransmitSRV      = 1,
        kSkyViewSRV       = 2,
        kTransmitUAV      = 3,
        kSkyViewUAV       = 4,
        // Consecutive SRV pair for sky render + env cubemap descriptor tables:
        //   slot 5 = SkyView SRV (t0 for render pass and cubemap pass)
        //   slot 6 = TransmittanceLUT SRV (t1 for render pass and cubemap pass)
        kRenderSkyViewSRV  = 5,
        kRenderTransmitSRV = 6,
        kEnvCubeUAV        = 7,   // UAV for writing the env cubemap array
        kHeapSize          = 8,
    };
};
