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
    float TileSize;           // mesh tile span in metres (== largest cascade tile)
    int MeshResolution = 128;  // base grid vertices per side before tessellation
};

class FFTWaterSurface
{
public:
    struct WaterTweaks
    {
        float windSpeed       = 10.0f;
        float windTheta       = 0.0f;   // radians, direction wind blows toward
        float amplitude       = 0.1f;
        float smallWaveCutoff = 0.01f;
        float choppiness      = 0.5f;   // λ: horizontal displacement scale [0,1]
        float timeScale       = 0.5f;   // slows or speeds up wave animation
        float maxTessellation = 32.0f;  // peak tessellation factor near camera
        float tessDistance    = 200.0f; // distance (m) at which tessellation reaches 1
        DirectX::XMFLOAT3 color = { 0.082f, 0.105f, 0.356f };
        bool  visible         = true;
        bool  wireframe       = false;
        int   tileCount       = 3;      // tiles per side; 1=single, 3=3×3 grid, etc.
        int   meshResolution  = 128;    // base grid vertices per side (runtime-changeable)
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

    // Demote displacement maps from PSR|NPSR back to NPSR on the graphics command list.
    // Call at end of every graphics frame before the next frame's async compute runs.
    void PrepareForCompute(CommandContext& ctx);

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

    // Matches WaterCommon.hlsli WaterConstants.
    struct WaterConstants
    {
        DirectX::XMFLOAT4X4 WorldMat;
        DirectX::XMFLOAT4X4 WorldViewProjMat;
        DirectX::XMFLOAT4   ColorMax;   // xyz = Color, w = MaxTessellation
        DirectX::XMFLOAT4   CascTess;   // xyz = 1/tileSize per cascade, w = TessDistance
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

    static constexpr int kNumCascades = 3;

    struct PhillipsParams
    {
        float windTheta, windSpeed, smallWaveCutoff, amplitude;
        bool operator==(const PhillipsParams& o) const
        {
            return windTheta == o.windTheta && windSpeed == o.windSpeed
                && smallWaveCutoff == o.smallWaveCutoff && amplitude == o.amplitude;
        }
    };

    // Per-cascade GPU state. Each cascade is an independent FFT ocean at a different
    // tile size; the render shader sums all cascades by world-space position. All
    // cascades share N (and therefore the twiddle table and ping-pong buffers).
    struct Cascade
    {
        float tileSize = 0.0f;

        PixelBuffer noise;       // R32G32_FLOAT       NxN  (Gaussian noise, SRV-only)
        PixelBuffer h0;          // R32G32B32A32_FLOAT NxN  (Phillips spectrum)
        PixelBuffer heightFreq;  // R32G32_FLOAT       NxN  (DynamicSpectrum output)
        PixelBuffer gradFreq;    // R32G32_FLOAT       NxN
        PixelBuffer dispFreq;    // R32G32_FLOAT       NxN
        PixelBuffer heightMap;   // R32G32_FLOAT       NxN  UAV→SRV (final height)
        PixelBuffer gradMap;     // R32G32_FLOAT       NxN  UAV→SRV (final gradient)
        PixelBuffer dispMap;     // R32G32_FLOAT       NxN  UAV→SRV (final displacement)

        ComPtr<ID3D12Resource>             noiseUpload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT noiseFootprint = {};
        std::vector<DirectX::XMFLOAT2>     noiseCpu;  // CPU mirror for buoyancy

        PhillipsParams lastPhillipsParams = {};
        bool           phillipsValid      = false;

        // Per-cascade descriptor-table base slots (assigned in BuildDescriptorTables).
        UINT phillipsTable = 0, dynSpecTable = 0;
        UINT initHeightTable = 0, initGradTable = 0, initDispTable = 0;
        UINT permuteHTable = 0, permuteGTable = 0, permuteDTable = 0;
    };

    // Runs Phillips + DynamicSpectrum + IFFT + Permute for one cascade.
    void RunCascadeFFT(CommandContext& ctx, LinearAllocator& alloc, Cascade& cs, float simTime);

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

    Cascade m_cascades[kNumCascades];

    // Shared GPU resources (independent of cascade; reused serially across cascades).
    PixelBuffer m_pingpong[2];   // R32G32_FLOAT       NxN     UAV+SRV (IFFT working buffers)
    PixelBuffer m_precomputed;   // R32G32B32A32_FLOAT log2N×N UAV+SRV (twiddle factors)

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

    bool m_precomputeDone     = false;  // twiddle factors only (shared, depends only on N)
    int  m_lastMeshResolution = 0;

    std::vector<float> m_spectrumPlot;  // size N/2+1; cascade 0 (swell), rebuilt on param change
    void BuildSpectrumPlot();

    // Descriptor-heap layout.
    //   Shared tables (reference only precomputed + ping-pong):   slots  0..17
    //   Per-cascade tables: base = kCascadeTablesBegin + c*kCascadeTableStride
    //   Render table (cascade maps grouped by type, then scene/env): kRenderTable
    enum : UINT
    {
        kPrecomputeTable    = 0,   // u0 = precomputed
        kIFFT0to1Table      = 6,   // s0=precomputed, s1=pingpong0, u0=pingpong1
        kIFFT1to0Table      = 12,  // s0=precomputed, s1=pingpong1, u0=pingpong0

        kCascadeTablesBegin = 24,
        kCascadeTableStride = 48,  // 8 tables × 6 slots
        //   +0  phillips    (s0=noise,      u0=h0)
        //   +6  dynspec     (s0=h0,         u0=hFreq,u1=gFreq,u2=dFreq)
        //   +12 initHeight  (s0=precomp,    s1=hFreq, u0=pingpong0)
        //   +18 initGrad
        //   +24 initDisp
        //   +30 permuteH    (s0=pingpong1,  u0=heightMap)
        //   +36 permuteG
        //   +42 permuteD

        // Render table: 3*kNumCascades map SRVs grouped [all height][all grad][all disp],
        // then sceneDepth, sceneColor, reflection cube.
        kRenderTable     = kCascadeTablesBegin + kNumCascades * kCascadeTableStride, // 168
        kRenderMapCount  = 3 * kNumCascades,                                          // 9
        kRenderSceneDepth = kRenderTable + kRenderMapCount,      // +9
        kRenderSceneColor = kRenderTable + kRenderMapCount + 1,  // +10
        kRenderEnvCube    = kRenderTable + kRenderMapCount + 2,  // +11

        kHeapSlotCount   = 192,
    };
};
