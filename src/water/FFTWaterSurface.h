#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

#include "gfx/CommandContext.h"
#include "gfx/GraphicsContext.h"
#include "gfx/LinearAllocator.h"
#include "gfx/PixelBuffer.h"
#include "gfx/GpuBuffer.h"
#include "IRenderer.h"

using Microsoft::WRL::ComPtr;

struct WaterDesc
{
    int N;
    int M;
    float TileSize;
};

class FFTWaterSurface
{
public:
    struct WaterTweaks
    {
        float windSpeed       = 31.0f;
        float windTheta       = 0.0f;   // radians, direction wind blows toward
        float amplitude       = 1.0f;
        float smallWaveCutoff = 0.01f;
        DirectX::XMFLOAT3 color = { 0.05f, 0.3f, 0.5f };
        bool  visible         = true;
    };

    WaterTweaks tweaks;

    FFTWaterSurface() = default;

    void Init(ID3D12Device* device, const WaterDesc& desc);

    const WaterDesc& GetDesc() const { return m_desc; }

    // Load six cube-map face images (px,nx,py,ny,pz,nz order) into the reflection slot.
    void LoadEnvironmentMap(ID3D12CommandQueue* queue, const std::string facePaths[6]);

    // time: elapsed seconds since app start, used to animate the spectrum.
    void Update(CommandContext& ctx, LinearAllocator& alloc, float time);

    // lightDir: world-space direction toward the light source.
    void Render(GraphicsContext& ctx, LinearAllocator& alloc, const View& view,
                const DirectX::XMFLOAT3& lightDir,
                D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSRV,
                D3D12_CPU_DESCRIPTOR_HANDLE sceneColorSRV);

private:
    // Matches FFTCommon.hlsli FFTParameters
    struct FFTParameters
    {
        uint32_t TextureSize;
        float    TileSize;
        float    RcpTileSize;
        float    _pad;
    };

    // Matches WaterCommon.hlsli WaterConstants (Color packed with TileSize in same float4)
    struct WaterConstants
    {
        DirectX::XMFLOAT4X4 WorldMat;
        DirectX::XMFLOAT4X4 WorldViewProjMat;
        DirectX::XMFLOAT3   Color;
        float                TileSize;
        float                RcpTileSize;
        float                _pad[3];
    };

    // Matches WaterCommon.hlsli FrameConstants
    struct WaterFrameConstants
    {
        DirectX::XMFLOAT3 CameraPosition;
        float             NearZ;
        DirectX::XMFLOAT3 LightDirection;
        float             FarZ;
    };

    void CreatePSOs();
    void CreateTextures();
    void CreateDescriptorHeap();
    void BuildDescriptorTables();
    void CreateMesh();
    void GenerateNoise();

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT slot) const;

    ID3D12Device* m_device = nullptr;
    WaterDesc     m_desc   = {};
    int           m_log2N  = 0;

    // Shader-visible descriptor heap owned by the water system.
    ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT                         m_descriptorSize = 0;

    ComPtr<ID3D12RootSignature> m_computeRootSig;
    ComPtr<ID3D12RootSignature> m_renderRootSig;

    // Compute PSOs (FFT pipeline)
    ComPtr<ID3D12PipelineState> PhillipsSpectrumPSO;
    ComPtr<ID3D12PipelineState> DynamicSpectrumPSO;
    ComPtr<ID3D12PipelineState> PrecomputePSO;
    ComPtr<ID3D12PipelineState> IFFTHorizonalStepPSO;
    ComPtr<ID3D12PipelineState> IFFTVerticalStepPSO;
    ComPtr<ID3D12PipelineState> PermutePSO;

    // Graphics PSO (water surface rendering)
    ComPtr<ID3D12PipelineState> WaterRenderPSO;

    // GPU textures — all as PixelBuffer for state-tracked barrier management
    PixelBuffer m_noise;         // R32G32_FLOAT   NxN  SRV (Gaussian noise, SRV-only)
    PixelBuffer m_h0;            // R32G32B32A32_FLOAT NxN UAV+SRV (Phillips spectrum)
    PixelBuffer m_heightFreq;    // R32G32_FLOAT   NxN  UAV (DynamicSpectrum output)
    PixelBuffer m_gradFreq;      // R32G32_FLOAT   NxN  UAV
    PixelBuffer m_dispFreq;      // R32G32_FLOAT   NxN  UAV
    PixelBuffer m_pingpong[2];   // R32G32_FLOAT   NxN  UAV+SRV (IFFT working buffers)
    PixelBuffer m_precomputed;   // R32G32B32A32_FLOAT log2N×N UAV+SRV (twiddle factors)
    PixelBuffer m_heightMap;     // R32G32_FLOAT   NxN  UAV→SRV (final spatial height)
    PixelBuffer m_gradMap;       // R32G32_FLOAT   NxN  UAV→SRV (final gradient)
    PixelBuffer m_dispMap;       // R32G32_FLOAT   NxN  UAV→SRV (final displacement)

    // Staging buffer for the one-time noise upload; no state transitions needed.
    ComPtr<ID3D12Resource>      m_noiseUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_noiseFootprint = {};

    // Environment cube map (reflection).
    ComPtr<ID3D12Resource>      m_envCubeMap;
    ComPtr<ID3D12Resource>      m_envCubeUpload;  // kept alive until GPU upload finishes

    // Water surface mesh
    VertexBuffer m_meshVertex;
    IndexBuffer  m_meshIndex;
    uint32_t     m_indexCount = 0;

    bool m_precomputeDone = false;

    // Heap slot indices for pre-built descriptor tables.
    enum : UINT
    {
        kNoiseSRV      = 0,
        kH0SRV         = 1,
        kHeightFreqSRV = 2,
        kGradFreqSRV   = 3,
        kDispFreqSRV   = 4,
        kPP0SRV        = 5,
        kPP1SRV        = 6,
        kPrecompSRV    = 7,
        kHeightMapSRV  = 8,
        kGradMapSRV    = 9,
        kDispMapSRV    = 10,
        kNullSRV       = 11,

        kNullUAV       = 12,
        kH0UAV         = 13,
        kHeightFreqUAV = 14,
        kGradFreqUAV   = 15,
        kDispFreqUAV   = 16,
        kPP0UAV        = 17,
        kPP1UAV        = 18,
        kPrecompUAV    = 19,
        kHeightMapUAV  = 20,
        kGradMapUAV    = 21,
        kDispMapUAV    = 22,

        kPhillipsTable   = 32,
        kDynSpecTable    = 38,
        kPrecomputeTable = 44,
        kInitHeightTable = 50,
        kInitGradTable   = 56,
        kInitDispTable   = 62,
        kIFFT0to1Table   = 68,
        kIFFT1to0Table   = 74,
        kPermuteHTable   = 80,
        kPermuteGTable   = 86,
        kPermuteDTable   = 92,
        kRenderTable     = 98,

        kHeapSlotCount   = 128,
    };
};
