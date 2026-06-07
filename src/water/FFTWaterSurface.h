#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <utility>
#include <vector>

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
    int MeshResolution = 16;  // base grid vertices per side before tessellation
};

class FFTWaterSurface
{
public:
    struct WaterTweaks
    {
        float windSpeed       = 10.0f;
        float windTheta       = 0.0f;   // radians, direction wind blows toward
        float amplitude       = 1.0f;
        float smallWaveCutoff = 0.01f;
        float choppiness      = 0.5f;   // λ: horizontal displacement scale [0,1]
        float timeScale       = 0.5f;   // slows or speeds up wave animation
        float maxTessellation = 32.0f;  // peak tessellation factor near camera
        float tessDistance    = 30.0f;  // distance (m) at which tessellation reaches 1
        DirectX::XMFLOAT3 color = { 0.082f, 0.105f, 0.356f };
        bool  visible         = true;
        bool  wireframe       = false;
        int   tileCount       = 3;      // tiles per side; 1=single, 3=3×3 grid, etc.
        int   meshResolution  = 16;     // base grid vertices per side (runtime-changeable)
        bool  tessellation    = true;   // use HS/DS tessellation; false = plain VS + triangle list
    };

    WaterTweaks tweaks;

    // CPU-side heightfield cache for buoyancy and other gameplay queries.
    // Call BuildHeightfield() once per frame; then query via GetHeightfield().
    struct CpuHeightfield
    {
        static constexpr int kGridN = 32;  // samples per tile side
        static constexpr int kMips  = 5;   // pyramid levels: 32→16→8→4→2

        struct MinMax { float mn = 0.0f, mx = 0.0f; };

        float  heights[kGridN * kGridN]         = {};
        // hiz[level] has (kGridN >> level)² valid entries; rest over-allocated for simplicity.
        MinMax hiz[kMips][kGridN * kGridN]      = {};
        float  globalMin                        = 0.0f;
        float  globalMax                        = 0.0f;

        // Build min/max pyramid from current heights[].
        void BuildPyramid();

        // Bilinearly interpolated height at any world position (tiles automatically).
        float Sample(float worldX, float worldZ, float tileSize) const;

        // Conservative min/max height within an XZ world-space bounding box.
        // Picks the finest pyramid level where the footprint fits in ≤ 9 cells.
        std::pair<float, float> QueryRange(
            float x0, float z0, float x1, float z1, float tileSize) const;
    };

    FFTWaterSurface() = default;

    void Init(ID3D12Device* device, const WaterDesc& desc, ID3D12CommandQueue* queue);

    // Rebuild the base mesh if tweaks.meshResolution changed since last build.
    // Call between WaitForFrame and BeginFrame to ensure no GPU work is in flight.
    void RebuildMeshIfNeeded();

    const WaterDesc& GetDesc() const { return m_desc; }

    // Load six cube-map face images (px,nx,py,ny,pz,nz order) into the reflection slot.
    void LoadEnvironmentMap(ID3D12CommandQueue* queue, const std::string facePaths[6]);

    // Override the reflection cubemap with a live resource (e.g., the atmosphere env cubemap).
    // Creates a TextureCube SRV directly into the render descriptor table.
    // Call once after Init(); the resource must remain valid for the lifetime of the surface.
    void SetEnvCubemapFromResource(ID3D12Device* device, ID3D12Resource* resource);

    // time: elapsed seconds since app start, used to animate the spectrum.
    void Update(CommandContext& ctx, LinearAllocator& alloc, float time);

    // CPU-side height evaluation using only the lowest `modes` frequency bins per axis.
    // Matches the GPU simulation exactly for those modes (same noise, same dispersion).
    // elapsedTime must be the same value passed to Update().
    // Naturally handles tiling — worldX/worldZ can be any value.
    float SampleHeightCPU(float worldX, float worldZ, float elapsedTime, int modes = 4) const;

    // Populate the CpuHeightfield cache for the current frame.
    // Call once per frame before any Sample/QueryRange calls.
    void BuildHeightfield(float elapsedTime, int modes = 4);
    const CpuHeightfield& GetHeightfield() const { return m_heightfield; }

    // Radially-averaged |h0(k)| spectrum, indexed by integer grid radius 0..N/2.
    // Rebuilt automatically whenever Phillips parameters change.
    const std::vector<float>& GetSpectrumPlot() const { return m_spectrumPlot; }

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
        float                MaxTessellation;
        float                TessDistance;
        float                _pad;
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
    void UploadNoiseSync(ID3D12CommandQueue* queue);

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

    // Graphics PSOs — tessellated (HS+DS) and flat (VS-only) variants
    ComPtr<ID3D12PipelineState> WaterRenderPSO;
    ComPtr<ID3D12PipelineState> WaterWireframePSO;
    ComPtr<ID3D12PipelineState> WaterFlatPSO;
    ComPtr<ID3D12PipelineState> WaterFlatWireframePSO;

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

    // CPU mirror of the noise texture — N×N Gaussian pairs, row-major.
    std::vector<DirectX::XMFLOAT2> m_noiseCpu;

    CpuHeightfield m_heightfield;

    // Environment cube map (reflection).
    ComPtr<ID3D12Resource>      m_envCubeMap;
    ComPtr<ID3D12Resource>      m_envCubeUpload;  // kept alive until GPU upload finishes

    // Water surface mesh
    VertexBuffer m_meshVertex;
    IndexBuffer  m_meshIndex;       // 4-index quad patches (tessellated path)
    IndexBuffer  m_meshIndexFlat;   // triangle list (non-tessellated path)
    uint32_t     m_indexCount     = 0;
    uint32_t     m_indexCountFlat = 0;

    struct PhillipsParams
    {
        float windTheta, windSpeed, smallWaveCutoff, amplitude;
        bool operator==(const PhillipsParams& o) const
        {
            return windTheta == o.windTheta && windSpeed == o.windSpeed
                && smallWaveCutoff == o.smallWaveCutoff && amplitude == o.amplitude;
        }
    };

    bool           m_precomputeDone     = false;  // twiddle factors only
    PhillipsParams m_lastPhillipsParams = {};     // zero — guaranteed != any real tweaks
    bool           m_phillipsValid      = false;
    int            m_lastMeshResolution = 0;

    std::vector<float> m_spectrumPlot;  // size N/2+1; rebuilt when Phillips params change
    void BuildSpectrumPlot();

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
