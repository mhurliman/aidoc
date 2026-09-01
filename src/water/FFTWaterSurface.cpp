#include "FFTWaterSurface.h"
#include "util/Assert.h"

#include <fstream>
#include <vector>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <DirectXMath.h>
#include <stb_image.h>

#undef min
#undef max

using namespace DirectX;

// ---------------------------------------------------------------------------
// File / path helpers
// ---------------------------------------------------------------------------

static std::string GetExeDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    auto pos = dir.find_last_of("\\/");
    return (pos != std::string::npos) ? dir.substr(0, pos + 1) : dir;
}

static std::vector<uint8_t> ReadShaderBlob(const char* name)
{
    std::string path = GetExeDir() + "shaders\\" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return {};
    auto size = (size_t)f.tellg();
    std::vector<uint8_t> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static ComPtr<ID3D12RootSignature> LoadRootSig(ID3D12Device* device, const char* blobName)
{
    auto blob = ReadShaderBlob(blobName);
    ComPtr<ID3D12RootSignature> rootSig;
    ASSERT_SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(),
                                                  IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

// ---------------------------------------------------------------------------
// Descriptor handle helpers
// ---------------------------------------------------------------------------

D3D12_GPU_DESCRIPTOR_HANDLE FFTWaterSurface::GpuHandle(UINT slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot * m_descriptorSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE FFTWaterSurface::CpuHandle(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * m_descriptorSize;
    return h;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void FFTWaterSurface::Init(ID3D12Device* device, const WaterDesc& desc, ID3D12CommandQueue* queue)
{
    m_device = device;
    m_desc   = desc;

    // Cascade tile sizes (metres): swell → mid waves → chop. Largest == mesh tile size.
    static const float kCascadeTileSizes[kNumCascades] = { 512.0f, 64.0f, 8.0f };
    for (int c = 0; c < kNumCascades; ++c)
        m_cascades[c].tileSize = kCascadeTileSizes[c];

    int n = desc.N;
    m_log2N = 0;
    while ((1 << m_log2N) < n)
        ++m_log2N;
    m_gradMips = m_log2N + 1;   // full mip chain: N, N/2, …, 1

    m_computeRootSig    = LoadRootSig(m_device, "fft_compute_rs.cso");
    m_downsampleRootSig = LoadRootSig(m_device, "downsample_rs.cso");
    m_renderRootSig     = LoadRootSig(m_device, "water_render_rs.cso");
    CreatePSOs();
    CreateTextures();
    CreateDescriptorHeap();
    m_vizSrvHeap.Init(m_device, (UINT)kNumCascades * 2);  // CPU SRVs for the debug images
    BuildDescriptorTables();
    tweaks.meshResolution = desc.MeshResolution;
    CreateMesh();
    m_lastMeshResolution = desc.MeshResolution;
    GenerateNoise();
    UploadNoiseSync(queue);
}

void FFTWaterSurface::UploadNoiseSync(ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12CommandAllocator>    tempAlloc;
    ComPtr<ID3D12GraphicsCommandList> tempCmd;
    ASSERT_SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&tempAlloc)));
    ASSERT_SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        tempAlloc.Get(), nullptr, IID_PPV_ARGS(&tempCmd)));

    for (int c = 0; c < kNumCascades; ++c)
    {
        Cascade& cs = m_cascades[c];

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource       = cs.noiseUpload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = cs.noiseFootprint;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource        = cs.noise.GetResource();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        tempCmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = cs.noise.GetResource();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tempCmd->ResourceBarrier(1, &barrier);
    }
    tempCmd->Close();

    ID3D12CommandList* lists[] = { tempCmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    ASSERT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ASSERT_SUCCEEDED(queue->Signal(fence.Get(), 1));
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    CloseHandle(ev);

    for (int c = 0; c < kNumCascades; ++c)
        m_cascades[c].noise.SetCurrentState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ---------------------------------------------------------------------------
// PSOs
// ---------------------------------------------------------------------------

static ComPtr<ID3D12PipelineState> CreateComputePSO(ID3D12Device* device,
                                                     ID3D12RootSignature* rootSig,
                                                     const char* shaderName)
{
    auto blob = ReadShaderBlob(shaderName);
    if (blob.empty())
        return nullptr;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSig;
    desc.CS = { blob.data(), blob.size() };

    ComPtr<ID3D12PipelineState> pso;
    ASSERT_SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

void FFTWaterSurface::CreatePSOs()
{
    PhillipsSpectrumPSO  = CreateComputePSO(m_device, m_computeRootSig.Get(), "phillips_spectrum_cs.cso");
    DownsamplePSO        = CreateComputePSO(m_device, m_downsampleRootSig.Get(), "downsample_cs.cso");
    VizPSO               = CreateComputePSO(m_device, m_downsampleRootSig.Get(), "water_viz_cs.cso");
    DynamicSpectrumPSO   = CreateComputePSO(m_device, m_computeRootSig.Get(), "dynamic_spectrum_cs.cso");
    PrecomputePSO        = CreateComputePSO(m_device, m_computeRootSig.Get(), "precompute_cs.cso");
    IFFTHorizonalStepPSO = CreateComputePSO(m_device, m_computeRootSig.Get(), "ifft_horz_cs.cso");
    IFFTVerticalStepPSO  = CreateComputePSO(m_device, m_computeRootSig.Get(), "ifft_vert_cs.cso");
    PermutePSO           = CreateComputePSO(m_device, m_computeRootSig.Get(), "permute_cs.cso");

    auto vsBlob     = ReadShaderBlob("water_surface_vs.cso");
    auto vsFullBlob = ReadShaderBlob("water_surface_vs_full.cso");
    auto hsBlob     = ReadShaderBlob("water_surface_hs.cso");
    auto dsBlob     = ReadShaderBlob("water_surface_ds.cso");
    auto psBlob     = ReadShaderBlob("water_surface_ps.cso");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode        = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode        = D3D12_CULL_MODE_NONE;
    rasterDesc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable    = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpsDesc = {};
    gpsDesc.pRootSignature        = m_renderRootSig.Get();
    gpsDesc.VS                    = { vsBlob.data(), vsBlob.size() };
    gpsDesc.HS                    = { hsBlob.data(), hsBlob.size() };
    gpsDesc.DS                    = { dsBlob.data(), dsBlob.size() };
    gpsDesc.PS                    = { psBlob.data(), psBlob.size() };
    gpsDesc.InputLayout           = { inputLayout, _countof(inputLayout) };
    gpsDesc.RasterizerState       = rasterDesc;
    gpsDesc.DepthStencilState     = dsDesc;
    gpsDesc.BlendState            = blendDesc;
    gpsDesc.SampleMask            = UINT_MAX;
    gpsDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    gpsDesc.NumRenderTargets      = 1;
    gpsDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpsDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    gpsDesc.SampleDesc.Count      = 1;

    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&gpsDesc, IID_PPV_ARGS(&WaterRenderPSO)));

    rasterDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    gpsDesc.RasterizerState = rasterDesc;
    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&gpsDesc, IID_PPV_ARGS(&WaterWireframePSO)));

    // Flat (non-tessellated) PSOs: VS_Full + PS, no HS/DS, triangle topology.
    gpsDesc.VS                    = { vsFullBlob.data(), vsFullBlob.size() };
    gpsDesc.HS                    = {};
    gpsDesc.DS                    = {};
    gpsDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    gpsDesc.RasterizerState = rasterDesc;
    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&gpsDesc, IID_PPV_ARGS(&WaterFlatPSO)));

    rasterDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    gpsDesc.RasterizerState = rasterDesc;
    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&gpsDesc, IID_PPV_ARGS(&WaterFlatWireframePSO)));

}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

void FFTWaterSurface::CreateTextures()
{
    int N = m_desc.N;
    int M = m_desc.M;
    constexpr D3D12_RESOURCE_FLAGS kUAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES kUAVState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Shared working buffers (reused serially across cascades).
    m_pingpong[0].Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_pingpong[1].Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_pingpong[0].SetDebugName(L"Water/PingPong0");
    m_pingpong[1].SetDebugName(L"Water/PingPong1");

    // Twiddle factor table: log2N columns, N rows. Depends only on N → shared.
    m_precomputed.Create(m_device, DXGI_FORMAT_R32G32B32A32_FLOAT, m_log2N, N, kUAV, kUAVState);
    m_precomputed.SetDebugName(L"Water/TwiddleFactors");

    for (int c = 0; c < kNumCascades; ++c)
    {
        Cascade& cs = m_cascades[c];

        // Noise: SRV-only, data arrives via CopyTextureRegion on first Update.
        cs.noise.Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N,
                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        cs.h0.Create        (m_device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, kUAV, kUAVState);
        cs.heightFreq.Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
        cs.gradFreq.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
        cs.dispFreq.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
        cs.heightMap.Create (m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState);
        // Gradient map carries a full mip chain (built each frame) so the PS can trilinear-sample
        // averaged slopes at distance — anti-aliases the specular/reflection sparkle.
        cs.gradMap.Create   (m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState, (UINT)m_gradMips);
        cs.dispMap.Create   (m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState);

        // Debug visualization images (written by the viz compute pass, sampled by ImGui).
        cs.vizSpectrum.Create(m_device, DXGI_FORMAT_R8G8B8A8_UNORM, N, N, kUAV, kUAVState);
        cs.vizHeight.Create  (m_device, DXGI_FORMAT_R8G8B8A8_UNORM, N, N, kUAV, kUAVState);

        wchar_t name[64];
        swprintf_s(name, L"Water/C%d/Noise",      c); cs.noise.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/H0",         c); cs.h0.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/HeightFreq", c); cs.heightFreq.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/GradFreq",   c); cs.gradFreq.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/DispFreq",   c); cs.dispFreq.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/HeightMap",  c); cs.heightMap.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/GradMap",    c); cs.gradMap.SetDebugName(name);
        swprintf_s(name, L"Water/C%d/DispMap",    c); cs.dispMap.SetDebugName(name);
    }
}

// ---------------------------------------------------------------------------
// Descriptor heap + tables
// ---------------------------------------------------------------------------

void FFTWaterSurface::CreateDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kHeapSlotCount;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ASSERT_SUCCEEDED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)));
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

static void CreateSRV2D(ID3D12Device* device, const PixelBuffer& buf,
                         D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format                  = buf.GetFormat();
    d.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels      = 1;
    device->CreateShaderResourceView(buf.GetResource(), &d, dest);
}

static void CreateUAV2D(ID3D12Device* device, const PixelBuffer& buf,
                         D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format        = buf.GetFormat();
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(buf.GetResource(), nullptr, &d, dest);
}

// SRV exposing the whole mip chain (for trilinear .Sample and .mips[] reads).
static void CreateSRV2DAllMips(ID3D12Device* device, const PixelBuffer& buf,
                               D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format                  = buf.GetFormat();
    d.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels     = buf.GetMipLevels();
    device->CreateShaderResourceView(buf.GetResource(), &d, dest);
}

// UAV of a single mip level (mip-chain downsample writes one mip at a time).
static void CreateUAV2DMip(ID3D12Device* device, const PixelBuffer& buf, UINT mip,
                           D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format               = buf.GetFormat();
    d.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice   = mip;
    device->CreateUnorderedAccessView(buf.GetResource(), nullptr, &d, dest);
}

static void CreateNullSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels      = 1;
    device->CreateShaderResourceView(nullptr, &d, dest);
}

static void CreateNullSRVCube(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURECUBE;
    d.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.TextureCube.MipLevels    = 1;
    device->CreateShaderResourceView(nullptr, &d, dest);
}

static void CreateNullUAV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(nullptr, nullptr, &d, dest);
}

void FFTWaterSurface::BuildDescriptorTables()
{
    // All descriptors are created directly at their final heap slots — no CopyDescriptorsSimple
    // from shader-visible heap, which is CPU write-only and cannot be used as a copy source.

    auto S = [&](PixelBuffer* buf, UINT slot) {
        if (buf) CreateSRV2D(m_device, *buf, CpuHandle(slot));
        else     CreateNullSRV(m_device, CpuHandle(slot));
    };
    auto U = [&](PixelBuffer* buf, UINT slot) {
        if (buf) CreateUAV2D(m_device, *buf, CpuHandle(slot));
        else     CreateNullUAV(m_device, CpuHandle(slot));
    };
    auto fillTable = [&](UINT base,
                          PixelBuffer* s0, PixelBuffer* s1, PixelBuffer* s2,
                          PixelBuffer* u0, PixelBuffer* u1, PixelBuffer* u2)
    {
        S(s0, base + 0); S(s1, base + 1); S(s2, base + 2);
        U(u0, base + 3); U(u1, base + 4); U(u2, base + 5);
    };

    // Shared tables (reference only precomputed + ping-pong buffers).
    fillTable(kPrecomputeTable, nullptr,        nullptr,        nullptr, &m_precomputed, nullptr, nullptr);
    fillTable(kIFFT0to1Table,   &m_precomputed, &m_pingpong[0], nullptr, &m_pingpong[1], nullptr, nullptr);
    fillTable(kIFFT1to0Table,   &m_precomputed, &m_pingpong[1], nullptr, &m_pingpong[0], nullptr, nullptr);

    // Per-cascade tables.
    for (int c = 0; c < kNumCascades; ++c)
    {
        Cascade& cs = m_cascades[c];
        UINT base = kCascadeTablesBegin + (UINT)c * kCascadeTableStride;
        cs.phillipsTable   = base + 0;
        cs.dynSpecTable    = base + 6;
        cs.initHeightTable = base + 12;
        cs.initGradTable   = base + 18;
        cs.initDispTable   = base + 24;
        cs.permuteHTable   = base + 30;
        cs.permuteGTable   = base + 36;
        cs.permuteDTable   = base + 42;

        fillTable(cs.phillipsTable,   &cs.noise,      nullptr,        nullptr, &cs.h0,        nullptr,     nullptr);
        fillTable(cs.dynSpecTable,    &cs.h0,         nullptr,        nullptr, &cs.heightFreq,&cs.gradFreq,&cs.dispFreq);
        fillTable(cs.initHeightTable, &m_precomputed, &cs.heightFreq, nullptr, &m_pingpong[0],nullptr,     nullptr);
        fillTable(cs.initGradTable,   &m_precomputed, &cs.gradFreq,   nullptr, &m_pingpong[0],nullptr,     nullptr);
        fillTable(cs.initDispTable,   &m_precomputed, &cs.dispFreq,   nullptr, &m_pingpong[0],nullptr,     nullptr);
        fillTable(cs.permuteHTable,   &m_pingpong[1], nullptr,        nullptr, &cs.heightMap, nullptr,     nullptr);
        fillTable(cs.permuteGTable,   &m_pingpong[1], nullptr,        nullptr, &cs.gradMap,   nullptr,     nullptr);
        fillTable(cs.permuteDTable,   &m_pingpong[1], nullptr,        nullptr, &cs.dispMap,   nullptr,     nullptr);
    }

    // Render table: cascade maps grouped by type [all height][all grad][all disp],
    // then two per-frame scene slots (filled in Render()) + reflection cube.
    for (int c = 0; c < kNumCascades; ++c) S(&m_cascades[c].heightMap, kRenderTable + 0 * kNumCascades + c);
    // Gradient SRV exposes the whole mip chain so the PS trilinear-samples it (anti-aliasing).
    for (int c = 0; c < kNumCascades; ++c)
        CreateSRV2DAllMips(m_device, m_cascades[c].gradMap, CpuHandle(kRenderTable + 1 * kNumCascades + c));
    for (int c = 0; c < kNumCascades; ++c) S(&m_cascades[c].dispMap,   kRenderTable + 2 * kNumCascades + c);
    CreateNullSRV    (m_device, CpuHandle(kRenderSceneDepth));
    CreateNullSRV    (m_device, CpuHandle(kRenderSceneColor));
    CreateNullSRVCube(m_device, CpuHandle(kRenderEnvCube));

    // Per-mip UAV views of each cascade's gradient map, for the downsample chain.
    for (int c = 0; c < kNumCascades; ++c)
        for (int m = 1; m < m_gradMips; ++m)
            CreateUAV2DMip(m_device, m_cascades[c].gradMap, (UINT)m,
                           CpuHandle(kMipGenBegin + c * kMaxGradMips + m));

    // Viz: dest UAVs (shader-visible, for the viz dispatch) + CPU SRVs (for ImGui copy).
    for (int c = 0; c < kNumCascades; ++c)
    {
        CreateUAV2D(m_device, m_cascades[c].vizSpectrum, CpuHandle(kVizBegin + c * 2 + 0));
        CreateUAV2D(m_device, m_cascades[c].vizHeight,   CpuHandle(kVizBegin + c * 2 + 1));

        UINT s0, s1;
        CreateSRV2D(m_device, m_cascades[c].vizSpectrum, m_vizSrvHeap.Allocate(s0));
        CreateSRV2D(m_device, m_cascades[c].vizHeight,   m_vizSrvHeap.Allocate(s1));
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE FFTWaterSurface::GetSpectrumVizSRV(int cascade) const
{
    return m_vizSrvHeap.GetCPUHandle((UINT)(cascade * 2 + 0));
}
D3D12_CPU_DESCRIPTOR_HANDLE FFTWaterSurface::GetHeightVizSRV(int cascade) const
{
    return m_vizSrvHeap.GetCPUHandle((UINT)(cascade * 2 + 1));
}

// ---------------------------------------------------------------------------
// Water mesh (tessellated grid)
// ---------------------------------------------------------------------------

void FFTWaterSurface::RebuildMeshIfNeeded()
{
    int desired = tweaks.meshResolution;
    if (desired == m_lastMeshResolution)
        return;
    desired = std::max(2, desired);
    tweaks.meshResolution = desired;
    m_desc.MeshResolution = desired;
    CreateMesh();
    m_lastMeshResolution = desired;
}

void FFTWaterSurface::CreateMesh()
{
    // Coarse base grid; tessellation fills in geometric detail near the camera.
    const int N = m_desc.MeshResolution;
    const int M = m_desc.MeshResolution;

    struct Vertex { float x, y, z, u, v; };
    std::vector<Vertex> verts;
    verts.reserve((size_t)N * M);
    for (int row = 0; row < M; ++row)
        for (int col = 0; col < N; ++col)
        {
            float u = col / float(N - 1);
            float v = row / float(M - 1);
            verts.push_back({ (u - 0.5f) * m_desc.TileSize, 0.0f,
                              (v - 0.5f) * m_desc.TileSize, u, v });
        }

    // 4-index quad patches: [TL, BL, TR, BR] per quad.
    std::vector<uint32_t> indices;
    indices.reserve((size_t)(N - 1) * (M - 1) * 4);
    for (int row = 0; row < M - 1; ++row)
        for (int col = 0; col < N - 1; ++col)
        {
            uint32_t tl = row * N + col;
            indices.push_back(tl);         // TL
            indices.push_back(tl + N);     // BL
            indices.push_back(tl + 1);     // TR
            indices.push_back(tl + N + 1); // BR
        }

    m_indexCount = static_cast<uint32_t>(indices.size());

    // Triangle-list index buffer for the non-tessellated path.
    std::vector<uint32_t> triIndices;
    triIndices.reserve((size_t)(N - 1) * (M - 1) * 6);
    for (int row = 0; row < M - 1; ++row)
        for (int col = 0; col < N - 1; ++col)
        {
            uint32_t tl = row * N + col;
            // Upper-left triangle
            triIndices.push_back(tl);
            triIndices.push_back(tl + N);
            triIndices.push_back(tl + 1);
            // Lower-right triangle
            triIndices.push_back(tl + 1);
            triIndices.push_back(tl + N);
            triIndices.push_back(tl + N + 1);
        }
    m_indexCountFlat = static_cast<uint32_t>(triIndices.size());

    m_meshVertex.CreateAndUpload(m_device, verts.data(),
                                  verts.size() * sizeof(Vertex), sizeof(Vertex));
    m_meshIndex.CreateAndUpload(m_device, indices.data(),
                                 indices.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
    m_meshIndexFlat.CreateAndUpload(m_device, triIndices.data(),
                                     triIndices.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
}

// ---------------------------------------------------------------------------
// Noise texture generation (Box-Muller Gaussian)
// ---------------------------------------------------------------------------

void FFTWaterSurface::GenerateNoise()
{
    int N = m_desc.N;

    D3D12_RESOURCE_DESC noiseDesc = {};
    noiseDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    noiseDesc.Width            = N;
    noiseDesc.Height           = N;
    noiseDesc.DepthOrArraySize = 1;
    noiseDesc.MipLevels        = 1;
    noiseDesc.Format           = DXGI_FORMAT_R32G32_FLOAT;
    noiseDesc.SampleDesc.Count = 1;
    noiseDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    for (int c = 0; c < kNumCascades; ++c)
    {
        Cascade& cs = m_cascades[c];

        UINT64 uploadSize;
        m_device->GetCopyableFootprints(&noiseDesc, 0, 1, 0,
                                        &cs.noiseFootprint, nullptr, nullptr, &uploadSize);

        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = uploadSize;
        bd.Height           = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ASSERT_SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr, IID_PPV_ARGS(&cs.noiseUpload)));

        cs.noiseCpu.resize((size_t)N * N);

        uint8_t* mapped = nullptr;
        cs.noiseUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        srand(12345 + c * 7919);  // distinct seed per cascade → decorrelated fields
        for (int y = 0; y < N; ++y)
        {
            float* row = reinterpret_cast<float*>(
                mapped + cs.noiseFootprint.Offset + (UINT64)y * cs.noiseFootprint.Footprint.RowPitch);
            for (int x = 0; x < N; ++x)
            {
                float u1 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
                float u2 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
                float mag = sqrtf(-2.0f * logf(u1));
                float gx = mag * cosf(6.28318530f * u2);
                float gy = mag * sinf(6.28318530f * u2);
                row[x * 2 + 0] = gx;
                row[x * 2 + 1] = gy;
                cs.noiseCpu[y * N + x] = { gx, gy };
            }
        }
        cs.noiseUpload->Unmap(0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Environment cube map
// ---------------------------------------------------------------------------

void FFTWaterSurface::LoadEnvironmentMap(ID3D12CommandQueue* queue, const std::string facePaths[6])
{
    // Load all 6 faces; require identical dimensions.
    int width = 0, height = 0;
    std::vector<std::vector<uint8_t>> pixels(6);
    for (int i = 0; i < 6; ++i)
    {
        int w, h, ch;
        uint8_t* data = stbi_load(facePaths[i].c_str(), &w, &h, &ch, 4);
        if (!data)
            return;
        if (i == 0) { width = w; height = h; }
        pixels[i].assign(data, data + (size_t)w * h * 4);
        stbi_image_free(data);
    }

    // Default heap resource: Texture2DArray with 6 slices.
    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC cubeDesc = {};
    cubeDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cubeDesc.Width            = (UINT64)width;
    cubeDesc.Height           = (UINT)height;
    cubeDesc.DepthOrArraySize = 6;
    cubeDesc.MipLevels        = 1;
    cubeDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    cubeDesc.SampleDesc.Count = 1;
    cubeDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ASSERT_SUCCEEDED(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &cubeDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_envCubeMap)));

    // Get upload footprints for all 6 subresources in one call.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(6);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&cubeDesc, 0, 6, 0,
        footprints.data(), nullptr, nullptr, &totalSize);

    D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC   uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = totalSize;
    uploadDesc.Height           = uploadDesc.DepthOrArraySize = uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ASSERT_SUCCEEDED(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_envCubeUpload)));

    uint8_t* mapped = nullptr;
    m_envCubeUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (int face = 0; face < 6; ++face)
    {
        const auto& fp = footprints[face];
        for (int row = 0; row < height; ++row)
            memcpy(mapped + fp.Offset + (UINT64)row * fp.Footprint.RowPitch,
                   pixels[face].data() + (size_t)row * width * 4,
                   (size_t)width * 4);
    }
    m_envCubeUpload->Unmap(0, nullptr);

    // Record copies + transition in a temporary command list.
    ComPtr<ID3D12CommandAllocator>    tempAlloc;
    ComPtr<ID3D12GraphicsCommandList> tempCmd;
    ASSERT_SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&tempAlloc)));
    ASSERT_SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        tempAlloc.Get(), nullptr, IID_PPV_ARGS(&tempCmd)));

    for (int face = 0; face < 6; ++face)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource        = m_envCubeMap.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = (UINT)face;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource       = m_envCubeUpload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[face];

        tempCmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_envCubeMap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tempCmd->ResourceBarrier(1, &barrier);
    tempCmd->Close();

    ID3D12CommandList* lists[] = { tempCmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    // CPU-side wait so the upload buffer can be freed later on (kept as member).
    ComPtr<ID3D12Fence> fence;
    ASSERT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ASSERT_SUCCEEDED(queue->Signal(fence.Get(), 1));
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    CloseHandle(ev);

    // Write a real TextureCube SRV into the render descriptor table slot.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_envCubeMap.Get(), &srvDesc,
                                       CpuHandle(kRenderEnvCube));
}

void FFTWaterSurface::SetEnvCubemapFromResource(ID3D12Device* device, ID3D12Resource* resource)
{
    // Retrieve the format from the resource desc so we don't need to hard-code it.
    D3D12_RESOURCE_DESC rdesc = resource->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = rdesc.Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels   = 1;
    device->CreateShaderResourceView(resource, &srvDesc, CpuHandle(kRenderEnvCube));
}

void FFTWaterSurface::PrepareForCompute(CommandContext& ctx)
{
    const D3D12_RESOURCE_STATES kNpsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (int c = 0; c < kNumCascades; ++c)
    {
        ctx.TransitionResource(m_cascades[c].heightMap, kNpsr);
        ctx.TransitionResource(m_cascades[c].gradMap,   kNpsr);
        ctx.TransitionResource(m_cascades[c].dispMap,   kNpsr);
    }
    ctx.FlushResourceBarriers();
}

// ---------------------------------------------------------------------------
// Spectral plot — radial power distribution for ImGui visualization
// ---------------------------------------------------------------------------

void FFTWaterSurface::BuildSpectrumPlot()
{
    // Plot cascade 0 (the swell cascade). X axis = |k| in grid units for that cascade.
    const Cascade& cs = m_cascades[0];
    if (cs.noiseCpu.empty())
        return;

    const float Pi    = 3.14159265f;
    const float g     = 9.81f;
    const int   N     = m_desc.N;
    const int   halfN = N / 2;
    const float DeltaK = 2.0f * Pi / cs.tileSize;
    const float L        = tweaks.windSpeed * tweaks.windSpeed / g;
    const float windCosT = cosf(tweaks.windTheta);
    const float windSinT = sinf(tweaks.windTheta);

    auto phillips = [&](float kx, float kz) -> float
    {
        float k2 = kx * kx + kz * kz;
        if (k2 < 1e-8f) return 0.0f;
        float kLen = sqrtf(k2);
        float kL2  = k2 * L * L;
        float kw   = (kx / kLen) * windCosT + (kz / kLen) * windSinT;
        float cutoff = expf(-k2 * tweaks.smallWaveCutoff * tweaks.smallWaveCutoff);
        float dir  = powf(kw * kw, tweaks.directionality);
        // Mirrors PhillipsSpectrum.hlsl, including the inverted sense: damping kw > 0 is what damps
        // the waves that TRAVEL upwind, because of the transform sign. See the note there.
        if (kw > 0.0f) dir *= tweaks.upwindAttenuation;
        return tweaks.amplitude * expf(-1.0f / kL2) * dir * cutoff / (k2 * k2);
    };

    const int binCount = halfN + 1;
    std::vector<float> sumH0(binCount, 0.0f);

    for (int nz = -halfN; nz < halfN; ++nz)
    {
        for (int nx = -halfN; nx < halfN; ++nx)
        {
            if (nx == 0 && nz == 0) continue;

            int bin = (int)roundf(sqrtf((float)(nx * nx + nz * nz)));
            if (bin <= 0 || bin >= binCount) continue;

            float kx = nx * DeltaK;
            float kz = nz * DeltaK;

            int tx = ((nx + halfN) % N + N) % N;
            int tz = ((nz + halfN) % N + N) % N;

            const auto& noise = cs.noiseCpu[tz * N + tx];
            float phiK = sqrtf(phillips(kx, kz) * 0.5f);
            float h0R  = noise.x * phiK;
            float h0I  = noise.y * phiK;
            sumH0[bin] += sqrtf(h0R * h0R + h0I * h0I);
        }
    }

    m_spectrumPlot = std::move(sumH0);
}

// ---------------------------------------------------------------------------
// CPU height sampling (low-frequency Tessendorf sum)
// ---------------------------------------------------------------------------

float FFTWaterSurface::SampleHeightCPU(float worldX, float worldZ, float elapsedTime, int modes) const
{
    const float Pi     = 3.14159265f;
    const float g      = 9.81f;
    const float time   = elapsedTime * tweaks.timeScale + 60.0f;
    const int   N      = m_desc.N;
    const int   halfN  = N / 2;

    const float L         = tweaks.windSpeed * tweaks.windSpeed / g;
    const float windCosT  = cosf(tweaks.windTheta);
    const float windSinT  = sinf(tweaks.windTheta);

    // Evaluate Phillips(k) — mirrors PhillipsSpectrum.hlsl
    auto phillips = [&](float kx, float kz) -> float
    {
        float k2 = kx * kx + kz * kz;
        if (k2 < 1e-8f) return 0.0f;
        float kLen = sqrtf(k2);
        float kL2  = k2 * L * L;
        float kw   = (kx / kLen) * windCosT + (kz / kLen) * windSinT;
        float cutoff = expf(-k2 * tweaks.smallWaveCutoff * tweaks.smallWaveCutoff);
        float dir  = powf(kw * kw, tweaks.directionality);
        // Mirrors PhillipsSpectrum.hlsl, including the inverted sense: damping kw > 0 is what damps
        // the waves that TRAVEL upwind, because of the transform sign. See the note there.
        if (kw > 0.0f) dir *= tweaks.upwindAttenuation;
        return tweaks.amplitude * expf(-1.0f / kL2) * dir * cutoff / (k2 * k2);
    };

    // Sum the low-frequency contribution of every cascade (matches the GPU render sum).
    float totalHeight = 0.0f;
    for (int c = 0; c < kNumCascades; ++c)
    {
        const Cascade& cs = m_cascades[c];
        if (cs.noiseCpu.empty())
            continue;

        const float DeltaK    = 2.0f * Pi / cs.tileSize;
        const float HalfScale = (1.0f / cs.tileSize) * 0.5f;

        float height = 0.0f;
        for (int nz = -modes; nz <= modes; ++nz)
        {
            for (int nx = -modes; nx <= modes; ++nx)
            {
                if (nx == 0 && nz == 0) continue;

                float kx = nx * DeltaK;
                float kz = nz * DeltaK;

                // Texture indices for k and -k, matching PhillipsSpectrum.hlsl addressing
                int tx  = ((  nx + halfN) % N + N) % N;
                int tz  = ((  nz + halfN) % N + N) % N;
                int mtx = (( -nx + halfN) % N + N) % N;
                int mtz = (( -nz + halfN) % N + N) % N;

                XMFLOAT2 noiseK    = cs.noiseCpu[tz  * N + tx];
                XMFLOAT2 noiseMinK = cs.noiseCpu[mtz * N + mtx];

                float phiK    = sqrtf(phillips( kx,  kz) * 0.5f);
                float phiMinK = sqrtf(phillips(-kx, -kz) * 0.5f);

                // h0(k) and conj(h0(-k)) — mirrors PhillipsSpectrum.hlsl
                float h0Kx  =  noiseK.x    * phiK;
                float h0Ky  =  noiseK.y    * phiK;
                float h0mKx =  noiseMinK.x * phiMinK;
                float h0mKy = -noiseMinK.y * phiMinK; // conjugate

                // Dispersion: omega = sqrt(|k| * g)
                float omega = sqrtf(sqrtf(kx*kx + kz*kz) * g);
                float eRe = cosf(omega * time);
                float eIm = sinf(omega * time);

                // h(k,t) = h0(k)*exp(i*omega*t) + conj(h0(-k))*exp(-i*omega*t)
                float hRe = (h0Kx*eRe - h0Ky*eIm) + (h0mKx*eRe + h0mKy*eIm);
                float hIm = (h0Kx*eIm + h0Ky*eRe) + (-h0mKx*eIm + h0mKy*eRe);

                // Accumulate real part of h(k,t) * exp(i*k·x)
                float kDotX = kx * worldX + kz * worldZ;
                height += hRe * cosf(kDotX) - hIm * sinf(kDotX);
            }
        }
        totalHeight += height * HalfScale;
    }

    return totalHeight;
}

void FFTWaterSurface::SampleHeightGrid(float originX, float originZ, float cell, int G,
                                       float elapsedTime, int modes, float* outH) const
{
    const float Pi     = 3.14159265f;
    const float g      = 9.81f;
    const float time   = elapsedTime * tweaks.timeScale + 60.0f;
    const int   N      = m_desc.N;
    const int   halfN  = N / 2;

    const float L         = tweaks.windSpeed * tweaks.windSpeed / g;
    const float windCosT  = cosf(tweaks.windTheta);
    const float windSinT  = sinf(tweaks.windTheta);

    // Phillips(k) — identical to SampleHeightCPU / PhillipsSpectrum.hlsl.
    auto phillips = [&](float kx, float kz) -> float
    {
        float k2 = kx * kx + kz * kz;
        if (k2 < 1e-8f) return 0.0f;
        float kLen = sqrtf(k2);
        float kL2  = k2 * L * L;
        float kw   = (kx / kLen) * windCosT + (kz / kLen) * windSinT;
        float cutoff = expf(-k2 * tweaks.smallWaveCutoff * tweaks.smallWaveCutoff);
        float dir  = powf(kw * kw, tweaks.directionality);
        // Mirrors PhillipsSpectrum.hlsl, including the inverted sense: damping kw > 0 is what damps
        // the waves that TRAVEL upwind, because of the transform sign. See the note there.
        if (kw > 0.0f) dir *= tweaks.upwindAttenuation;
        return tweaks.amplitude * expf(-1.0f / kL2) * dir * cutoff / (k2 * k2);
    };

    const int GG = G * G;
    for (int i = 0; i < GG; ++i) outH[i] = 0.0f;

    for (int c = 0; c < kNumCascades; ++c)
    {
        const Cascade& cs = m_cascades[c];
        if (cs.noiseCpu.empty()) continue;

        const float DeltaK    = 2.0f * Pi / cs.tileSize;
        const float HalfScale = (1.0f / cs.tileSize) * 0.5f;   // height: HeightMap * (rcp*0.5)

        for (int nz = -modes; nz <= modes; ++nz)
            for (int nx = -modes; nx <= modes; ++nx)
            {
                if (nx == 0 && nz == 0) continue;

                float kx = nx * DeltaK;
                float kz = nz * DeltaK;

                int tx  = ((  nx + halfN) % N + N) % N;
                int tz  = ((  nz + halfN) % N + N) % N;
                int mtx = (( -nx + halfN) % N + N) % N;
                int mtz = (( -nz + halfN) % N + N) % N;
                XMFLOAT2 noiseK    = cs.noiseCpu[tz  * N + tx];
                XMFLOAT2 noiseMinK = cs.noiseCpu[mtz * N + mtx];

                float phiK    = sqrtf(phillips( kx,  kz) * 0.5f);
                float phiMinK = sqrtf(phillips(-kx, -kz) * 0.5f);
                float h0Kx  =  noiseK.x    * phiK;
                float h0Ky  =  noiseK.y    * phiK;
                float h0mKx =  noiseMinK.x * phiMinK;
                float h0mKy = -noiseMinK.y * phiMinK; // conjugate

                float omega = sqrtf(sqrtf(kx*kx + kz*kz) * g);
                float eRe = cosf(omega * time);
                float eIm = sinf(omega * time);

                // Raw complex height amplitude h(k,t).
                float hRe = (h0Kx*eRe - h0Ky*eIm) + (h0mKx*eRe + h0mKy*eIm);
                float hIm = (h0Kx*eIm + h0Ky*eRe) + (-h0mKx*eIm + h0mKy*eRe);
                // Height contribution factors: Re(h·e^{ikx}) = hRe·re - hIm·im.
                float hReS = hRe * HalfScale, hImS = hIm * HalfScale;

                // March e^{i k·x} across the regular grid by fixed per-column/row complex rotor.
                float cx = cosf(kx * cell), sx = sinf(kx * cell);
                float cz = cosf(kz * cell), sz = sinf(kz * cell);
                float base = kx * originX + kz * originZ;
                float rowRe = cosf(base), rowIm = sinf(base);
                for (int j = 0; j < G; ++j)
                {
                    float re = rowRe, im = rowIm;
                    int rowBase = j * G;
                    for (int i = 0; i < G; ++i)
                    {
                        outH[rowBase + i] += hReS * re - hImS * im;   // Re(h · e^{i k·x})
                        float nre = re * cx - im * sx;
                        im = re * sx + im * cx;
                        re = nre;
                    }
                    float nrre = rowRe * cz - rowIm * sz;
                    rowIm = rowRe * sz + rowIm * cz;
                    rowRe = nrre;
                }
            }
    }
}

void FFTWaterSurface::SampleSurfaceGrid(float originX, float originZ, float cell, int G,
                                        float elapsedTime, int modes,
                                        float* outH, float* outDx, float* outDz) const
{
    const float Pi     = 3.14159265f;
    const float g      = 9.81f;
    const float time   = elapsedTime * tweaks.timeScale + 60.0f;
    const int   N      = m_desc.N;
    const int   halfN  = N / 2;
    const float chop   = tweaks.choppiness;

    const float L         = tweaks.windSpeed * tweaks.windSpeed / g;
    const float windCosT  = cosf(tweaks.windTheta);
    const float windSinT  = sinf(tweaks.windTheta);

    auto phillips = [&](float kx, float kz) -> float
    {
        float k2 = kx * kx + kz * kz;
        if (k2 < 1e-8f) return 0.0f;
        float kLen = sqrtf(k2);
        float kL2  = k2 * L * L;
        float kw   = (kx / kLen) * windCosT + (kz / kLen) * windSinT;
        float cutoff = expf(-k2 * tweaks.smallWaveCutoff * tweaks.smallWaveCutoff);
        float dir  = powf(kw * kw, tweaks.directionality);
        // Mirrors PhillipsSpectrum.hlsl, including the inverted sense: damping kw > 0 is what damps
        // the waves that TRAVEL upwind, because of the transform sign. See the note there.
        if (kw > 0.0f) dir *= tweaks.upwindAttenuation;
        return tweaks.amplitude * expf(-1.0f / kL2) * dir * cutoff / (k2 * k2);
    };

    const int GG = G * G;
    for (int i = 0; i < GG; ++i) { outH[i] = 0.0f; outDx[i] = 0.0f; outDz[i] = 0.0f; }

    for (int c = 0; c < kNumCascades; ++c)
    {
        const Cascade& cs = m_cascades[c];
        if (cs.noiseCpu.empty()) continue;

        const float DeltaK    = 2.0f * Pi / cs.tileSize;
        const float HalfScale = (1.0f / cs.tileSize) * 0.5f;   // height:  HeightMap * (rcp*0.5)
        const float DispScale = (1.0f / cs.tileSize);          // disp:    DisplacementMap * rcp

        for (int nz = -modes; nz <= modes; ++nz)
            for (int nx = -modes; nx <= modes; ++nx)
            {
                if (nx == 0 && nz == 0) continue;

                float kx = nx * DeltaK;
                float kz = nz * DeltaK;
                float kLen = sqrtf(kx*kx + kz*kz);

                int tx  = ((  nx + halfN) % N + N) % N;
                int tz  = ((  nz + halfN) % N + N) % N;
                int mtx = (( -nx + halfN) % N + N) % N;
                int mtz = (( -nz + halfN) % N + N) % N;
                XMFLOAT2 noiseK    = cs.noiseCpu[tz  * N + tx];
                XMFLOAT2 noiseMinK = cs.noiseCpu[mtz * N + mtx];

                float phiK    = sqrtf(phillips( kx,  kz) * 0.5f);
                float phiMinK = sqrtf(phillips(-kx, -kz) * 0.5f);
                float h0Kx  =  noiseK.x    * phiK;
                float h0Ky  =  noiseK.y    * phiK;
                float h0mKx =  noiseMinK.x * phiMinK;
                float h0mKy = -noiseMinK.y * phiMinK;

                float omega = sqrtf(kLen * g);
                float eRe = cosf(omega * time);
                float eIm = sinf(omega * time);

                float hRe = (h0Kx*eRe - h0Ky*eIm) + (h0mKx*eRe + h0mKy*eIm);
                float hIm = (h0Kx*eIm + h0Ky*eRe) + (-h0mKx*eIm + h0mKy*eRe);
                float hReS = hRe * HalfScale, hImS = hIm * HalfScale;
                // Displacement: GPU packs Offset = choppiness·i·k·h/|k| (complex), IFFTs it, so
                // Dx = Re(Σ Offset·e^{ikx}), Dz = Im(...). Offset = (A + iB), matching
                // DynamicSpectrum.hlsl's ComplexMult(k, i·h):
                float dK = DispScale * chop / kLen;
                float A  = dK * (-kx * hIm - kz * hRe);
                float B  = dK * ( kx * hRe - kz * hIm);

                float cx = cosf(kx * cell), sx = sinf(kx * cell);
                float cz = cosf(kz * cell), sz = sinf(kz * cell);
                float base = kx * originX + kz * originZ;
                float rowRe = cosf(base), rowIm = sinf(base);
                for (int j = 0; j < G; ++j)
                {
                    float re = rowRe, im = rowIm;
                    int rowBase = j * G;
                    for (int i = 0; i < G; ++i)
                    {
                        outH[rowBase + i]  += hReS * re - hImS * im;   // Re(h · e^{i k·x})
                        outDx[rowBase + i] += A * re - B * im;         // Re(Offset · e^{i k·x})
                        outDz[rowBase + i] += A * im + B * re;         // Im(Offset · e^{i k·x})
                        float nre = re * cx - im * sx;
                        im = re * sx + im * cx;
                        re = nre;
                    }
                    float nrre = rowRe * cz - rowIm * sz;
                    rowIm = rowRe * sz + rowIm * cz;
                    rowRe = nrre;
                }
            }
    }
}

// ---------------------------------------------------------------------------
// CpuHeightfield — pyramid build, bilinear sample, hierarchical range query
// ---------------------------------------------------------------------------

void FFTWaterSurface::CpuHeightfield::BuildPyramid()
{
    // Level 0: one min/max cell per height sample
    for (int i = 0; i < kGridN * kGridN; ++i)
        hiz[0][i] = { heights[i], heights[i] };

    // Each subsequent level merges 2×2 blocks from the level below
    for (int lvl = 1; lvl < kMips; ++lvl)
    {
        int n  = kGridN >> lvl;
        int pn = n * 2;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
            {
                float mn = FLT_MAX, mx = -FLT_MAX;
                for (int dz = 0; dz < 2; ++dz)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const auto& p = hiz[lvl - 1][(z * 2 + dz) * pn + (x * 2 + dx)];
                        mn = std::min(mn, p.mn);
                        mx = std::max(mx, p.mx);
                    }
                hiz[lvl][z * n + x] = { mn, mx };
            }
    }

    // Global bounds from the coarsest level (2×2 = 4 cells)
    globalMin = FLT_MAX;
    globalMax = -FLT_MAX;
    const int cn = kGridN >> (kMips - 1);
    for (int i = 0; i < cn * cn; ++i)
    {
        globalMin = std::min(globalMin, hiz[kMips - 1][i].mn);
        globalMax = std::max(globalMax, hiz[kMips - 1][i].mx);
    }
}

float FFTWaterSurface::CpuHeightfield::Sample(
    float worldX, float worldZ, float tileSize) const
{
    float u = fmodf(worldX / tileSize, 1.0f); if (u < 0.0f) u += 1.0f;
    float v = fmodf(worldZ / tileSize, 1.0f); if (v < 0.0f) v += 1.0f;

    float gx = u * kGridN;
    float gz = v * kGridN;
    int   x0 = (int)gx % kGridN,  x1 = (x0 + 1) % kGridN;
    int   z0 = (int)gz % kGridN,  z1 = (z0 + 1) % kGridN;
    float fx = gx - floorf(gx),   fz = gz - floorf(gz);

    return heights[z0 * kGridN + x0] * (1.0f - fx) * (1.0f - fz)
         + heights[z0 * kGridN + x1] *         fx  * (1.0f - fz)
         + heights[z1 * kGridN + x0] * (1.0f - fx) *         fz
         + heights[z1 * kGridN + x1] *         fx  *         fz;
}

std::pair<float, float> FFTWaterSurface::CpuHeightfield::QueryRange(
    float x0, float z0, float x1, float z1, float tileSize) const
{
    float extX = x1 - x0, extZ = z1 - z0;
    if (extX >= tileSize || extZ >= tileSize)
        return { globalMin, globalMax };

    // Walk from finest to coarsest; stop at the first level where the footprint
    // fits within a 3×3 cell block (≤ 9 lookups).
    int lvl = kMips - 1;
    for (int l = 0; l < kMips; ++l)
    {
        int n      = kGridN >> l;
        int cellsX = (int)ceilf(extX / tileSize * n) + 2;
        int cellsZ = (int)ceilf(extZ / tileSize * n) + 2;
        if (cellsX <= 3 && cellsZ <= 3) { lvl = l; break; }
    }

    int   n   = kGridN >> lvl;
    float uf0 = fmodf(x0 / tileSize, 1.0f); if (uf0 < 0.0f) uf0 += 1.0f;
    float vf0 = fmodf(z0 / tileSize, 1.0f); if (vf0 < 0.0f) vf0 += 1.0f;

    int cx0 = (int)(uf0 * n);
    int cz0 = (int)(vf0 * n);
    int cx1 = cx0 + (int)ceilf(extX / tileSize * n) + 1;
    int cz1 = cz0 + (int)ceilf(extZ / tileSize * n) + 1;

    float mn = FLT_MAX, mx = -FLT_MAX;
    for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const auto& cell = hiz[lvl][(cz % n) * n + (cx % n)];
            mn = std::min(mn, cell.mn);
            mx = std::max(mx, cell.mx);
        }
    return { mn, mx };
}

// ---------------------------------------------------------------------------
// BuildHeightfield — populate the CpuHeightfield cache for the current frame
// ---------------------------------------------------------------------------

void FFTWaterSurface::BuildHeightfield(float elapsedTime, int modes)
{
    const int kN = CpuHeightfield::kGridN;
    for (int zi = 0; zi < kN; ++zi)
        for (int xi = 0; xi < kN; ++xi)
        {
            float wx = float(xi) / kN * m_desc.TileSize;
            float wz = float(zi) / kN * m_desc.TileSize;
            m_heightfield.heights[zi * kN + xi] = SampleHeightCPU(wx, wz, elapsedTime, modes);
        }
    m_heightfield.BuildPyramid();
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void FFTWaterSurface::Update(CommandContext& ctx, LinearAllocator& alloc, float time)
{
    auto* cmd = ctx.GetCommandList();
    int N = m_desc.N;
    UINT groupsN = (UINT)(N / 8);

    ctx.SetDescriptorHeap(m_heap.Get());

    // ---- Twiddle factors — one-time precomputation (depends only on N; shared) ----
    if (!m_precomputeDone)
    {
        auto fftAlloc   = alloc.Allocate(sizeof(FFTParameters));
        FFTParameters fftParams = { (uint32_t)N, m_desc.TileSize, 1.0f / m_desc.TileSize };
        memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));
        auto dummyAlloc = alloc.Allocate(256);

        cmd->SetComputeRootSignature(m_computeRootSig.Get());
        ctx.SetPipelineState(PrecomputePSO.Get());
        cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
        cmd->SetComputeRootConstantBufferView(1, dummyAlloc.gpuAddress);
        ctx.FlushResourceBarriers();
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(kPrecomputeTable));
        cmd->Dispatch((UINT)m_log2N, groupsN / 2, 1);

        ctx.UAVBarrier(m_precomputed);
        ctx.TransitionResource(m_precomputed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_precomputeDone = true;
    }

    // Common simulation clock shared by all cascades.
    float simTime = time * tweaks.timeScale + 60.0f;

    for (int c = 0; c < kNumCascades; ++c)
        RunCascadeFFT(ctx, alloc, m_cascades[c], simTime);

    if (m_readbackEnabled) CaptureHeightReadback(ctx);

    ctx.FlushResourceBarriers();
}

// Copy this frame's height maps into the readback ring, and lift the slot that has retired into
// m_readbackHeights. Called at the end of Update, where the maps have just been written and have
// not yet been transitioned for rendering.
void FFTWaterSurface::CaptureHeightReadback(CommandContext& ctx)
{
    const int N = m_desc.N, M = m_desc.M;

    D3D12_RESOURCE_DESC texDesc = m_cascades[0].heightMap.GetResource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 rowBytes = 0, totalBytes = 0;
    UINT   numRows  = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowBytes,
                                    &totalBytes);
    m_readbackRowPitch = footprint.Footprint.RowPitch;

    // Allocated on first use rather than at startup: this is a diagnostic and most runs never ask.
    if (!m_heightReadback[0])
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width            = totalBytes * kNumCascades;
        bufDesc.Height           = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels        = 1;
        bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (int i = 0; i < kReadbackSlots; ++i)
        {
            ASSERT_SUCCEEDED(m_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&m_heightReadback[i])));
        }
        m_readbackHeights.assign(static_cast<size_t>(N) * M * kNumCascades, 0.0f);
        m_readbackWarmup = 0;
        m_readbackValid  = false;
    }

    for (int c = 0; c < kNumCascades; ++c)
        ctx.TransitionResource(m_cascades[c].heightMap, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.FlushResourceBarriers();

    auto* cmd = ctx.GetCommandList();
    for (int c = 0; c < kNumCascades; ++c)
    {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_cascades[c].heightMap.GetResource();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_heightReadback[m_readbackSlot].Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        dst.PlacedFootprint.Offset = totalBytes * c;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    for (int c = 0; c < kNumCascades; ++c)
        ctx.TransitionResource(m_cascades[c].heightMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Read the slot the ring is about to reuse NEXT: it is the oldest, so the GPU has long since
    // finished with it. Reading the slot just written would be a race; waiting for it would be a
    // stall. The cost is that the number on screen is a few frames old, which for a surface that
    // moves at wave frequencies is invisible.
    m_readbackSlot = (m_readbackSlot + 1) % kReadbackSlots;
    if (m_readbackWarmup < kReadbackSlots) { ++m_readbackWarmup; return; }

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes * kNumCascades) };
    if (FAILED(m_heightReadback[m_readbackSlot]->Map(0, &readRange, &mapped))) return;

    // R32G32; the surface reads .x, so only that channel is kept.
    for (int c = 0; c < kNumCascades; ++c)
    {
        const auto* base = static_cast<const uint8_t*>(mapped) + totalBytes * c;
        for (int y = 0; y < M; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(base + m_readbackRowPitch * y);
            float* out = &m_readbackHeights[(static_cast<size_t>(c) * M + y) * N];
            for (int x = 0; x < N; ++x) out[x] = row[x * 2];
        }
    }

    D3D12_RANGE noWrite = { 0, 0 };
    m_heightReadback[m_readbackSlot]->Unmap(0, &noWrite);
    m_readbackValid = true;
}

bool FFTWaterSurface::SampleHeightGPU(float worldX, float worldZ, float& outHeight) const
{
    if (!m_readbackValid) return false;

    const int N = m_desc.N, M = m_desc.M;
    float total = 0.0f;

    for (int c = 0; c < kNumCascades; ++c)
    {
        if (!tweaks.cascadeEnabled[c]) continue;
        const float rcp = 1.0f / m_cascades[c].tileSize;

        // Same uv and the same bilinear filter the domain shader gets from its linear sampler,
        // with WRAP addressing - reproducing the FETCH rather than the maths, which is the point.
        const float u = worldX * rcp * static_cast<float>(N) - 0.5f;
        const float v = worldZ * rcp * static_cast<float>(M) - 0.5f;
        const int   x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
        const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);

        auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
        const int xa = wrap(x0, N), xb = wrap(x0 + 1, N);
        const int ya = wrap(y0, M), yb = wrap(y0 + 1, M);

        const float* plane = &m_readbackHeights[static_cast<size_t>(c) * M * N];
        const float h00 = plane[static_cast<size_t>(ya) * N + xa];
        const float h10 = plane[static_cast<size_t>(ya) * N + xb];
        const float h01 = plane[static_cast<size_t>(yb) * N + xa];
        const float h11 = plane[static_cast<size_t>(yb) * N + xb];

        const float h = (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fy)
                      + (h01 * (1.0f - fx) + h11 * fx) * fy;

        total += h * (rcp * 0.5f);   // the domain shader's amplitude scaling
    }

    outHeight = total;
    return true;
}

// Phillips + DynamicSpectrum + IFFT + Permute for a single cascade. The ping-pong and
// twiddle buffers are shared and reused serially, so cascades must run sequentially.
void FFTWaterSurface::RunCascadeFFT(CommandContext& ctx, LinearAllocator& alloc,
                                    Cascade& cs, float simTime)
{
    auto* cmd = ctx.GetCommandList();
    int N = m_desc.N;
    UINT groupsN = (UINT)(N / 8);

    const FFTParameters fftParams = { (uint32_t)N, cs.tileSize, 1.0f / cs.tileSize };

    // ---- Phillips spectrum — re-run whenever spectral parameters change ----
    // Noise is already in NON_PIXEL_SHADER_RESOURCE state (uploaded synchronously in Init).
    {
        PhillipsParams currentParams = { tweaks.windTheta, tweaks.windSpeed,
                                         tweaks.smallWaveCutoff, tweaks.amplitude,
                                         tweaks.directionality, tweaks.upwindAttenuation };
        if (!cs.phillipsValid || !(currentParams == cs.lastPhillipsParams))
        {
            // h0 starts as UAV on first frame; on re-runs it's NPSR — transition back to UAV.
            ctx.TransitionResource(cs.h0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            struct PhillipsGlobals
            {
                float windTheta, windSpeed, smallWaveCutoff, amplitude, dirExponent;
                float upwindAttenuation;
            };
            auto fftAlloc  = alloc.Allocate(sizeof(FFTParameters));
            memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));

            auto globAlloc = alloc.Allocate(sizeof(PhillipsGlobals));
            PhillipsGlobals globals = { tweaks.windTheta, tweaks.windSpeed,
                                        tweaks.smallWaveCutoff, tweaks.amplitude,
                                        tweaks.directionality, tweaks.upwindAttenuation };
            memcpy(globAlloc.cpuAddress, &globals, sizeof(globals));

            cmd->SetComputeRootSignature(m_computeRootSig.Get());
            ctx.SetPipelineState(PhillipsSpectrumPSO.Get());
            cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, globAlloc.gpuAddress);
            ctx.FlushResourceBarriers();
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(cs.phillipsTable));
            cmd->Dispatch(groupsN, groupsN, 1);

            ctx.UAVBarrier(cs.h0);
            ctx.TransitionResource(cs.h0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            cs.lastPhillipsParams = currentParams;
            cs.phillipsValid      = true;
            if (&cs == &m_cascades[0])
                BuildSpectrumPlot();  // plot the swell cascade
        }
    }

    // ---- Transition this cascade's final maps to UAV for this frame's writes ----
    ctx.TransitionResource(cs.heightMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.TransitionResource(cs.gradMap,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.TransitionResource(cs.dispMap,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // heightFreq is left NPSR by last frame's graphics viz read — reclaim it for the write below.
    ctx.TransitionResource(cs.heightFreq, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- Dynamic spectrum (per-frame) ----
    {
        auto fftAlloc  = alloc.Allocate(sizeof(FFTParameters));
        memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));

        auto timeAlloc = alloc.Allocate(256);
        struct DynSpecGlobals { float SimulationTime; float Choppiness; int ModeLimit; };
        DynSpecGlobals dynGlobals = { simTime, tweaks.choppiness, tweaks.modeLimit };
        memcpy(timeAlloc.cpuAddress, &dynGlobals, sizeof(dynGlobals));

        cmd->SetComputeRootSignature(m_computeRootSig.Get());
        ctx.SetPipelineState(DynamicSpectrumPSO.Get());
        cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
        cmd->SetComputeRootConstantBufferView(1, timeAlloc.gpuAddress);
        ctx.FlushResourceBarriers();
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(cs.dynSpecTable));
        cmd->Dispatch(groupsN, groupsN, 1);
    }

    ctx.UAVBarrier(cs.heightFreq);
    ctx.UAVBarrier(cs.gradFreq);
    ctx.UAVBarrier(cs.dispFreq);

    // ---- IFFT + Permute for each channel ----
    struct ChannelDesc { UINT initTable; PixelBuffer* srcFreq; UINT permuteTable; PixelBuffer* finalMap; };
    ChannelDesc channels[3] = {
        { cs.initHeightTable, &cs.heightFreq, cs.permuteHTable, &cs.heightMap },
        { cs.initGradTable,   &cs.gradFreq,   cs.permuteGTable, &cs.gradMap   },
        { cs.initDispTable,   &cs.dispFreq,   cs.permuteDTable, &cs.dispMap   },
    };

    for (auto& ch : channels)
    {
        ctx.TransitionResource(*ch.srcFreq, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        for (int step = 0; step < m_log2N; ++step)
        {
            int writeIdx  = step % 2;
            UINT tableBase = (step == 0) ? ch.initTable
                           : ((writeIdx == 1) ? kIFFT0to1Table : kIFFT1to0Table);

            auto globAlloc  = alloc.Allocate(256);
            uint32_t stepVal = (uint32_t)step;
            memcpy(globAlloc.cpuAddress, &stepVal, sizeof(stepVal));
            auto dummyAlloc = alloc.Allocate(256);

            ctx.SetPipelineState(IFFTHorizonalStepPSO.Get());
            cmd->SetComputeRootConstantBufferView(0, globAlloc.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, dummyAlloc.gpuAddress);
            ctx.FlushResourceBarriers();
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(tableBase));
            cmd->Dispatch(groupsN, groupsN, 1);

            ctx.UAVBarrier(m_pingpong[writeIdx]);
            ctx.TransitionResource(m_pingpong[writeIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (step >= 1 && step + 1 < m_log2N)
                ctx.TransitionResource(m_pingpong[(step + 1) % 2], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        {
            int lastHorzWrite  = (m_log2N - 1) % 2;
            int firstVertWrite = m_log2N % 2;
            if (firstVertWrite != lastHorzWrite)
                ctx.TransitionResource(m_pingpong[firstVertWrite], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        for (int step = 0; step < m_log2N; ++step)
        {
            int readIdx  = (m_log2N - 1 + step) % 2;
            int writeIdx = (m_log2N + step) % 2;

            UINT tableBase = (readIdx == 0) ? kIFFT0to1Table : kIFFT1to0Table;

            auto globAlloc  = alloc.Allocate(256);
            uint32_t stepVal = (uint32_t)step;
            memcpy(globAlloc.cpuAddress, &stepVal, sizeof(stepVal));
            auto dummyAlloc = alloc.Allocate(256);

            ctx.SetPipelineState(IFFTVerticalStepPSO.Get());
            cmd->SetComputeRootConstantBufferView(0, globAlloc.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, dummyAlloc.gpuAddress);
            ctx.FlushResourceBarriers();
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(tableBase));
            cmd->Dispatch(groupsN, groupsN, 1);

            ctx.UAVBarrier(m_pingpong[writeIdx]);
            ctx.TransitionResource(m_pingpong[writeIdx], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (step + 1 < m_log2N)
                ctx.TransitionResource(m_pingpong[(writeIdx + 1) % 2], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // Permute: reads PingPong[1] (SRV), writes final map (UAV).
        {
            auto dummy0 = alloc.Allocate(256);
            auto dummy1 = alloc.Allocate(256);

            ctx.SetPipelineState(PermutePSO.Get());
            cmd->SetComputeRootConstantBufferView(0, dummy0.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, dummy1.gpuAddress);
            ctx.FlushResourceBarriers();
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(ch.permuteTable));
            cmd->Dispatch(groupsN, groupsN, 1);
            ctx.UAVBarrier(*ch.finalMap);
        }

        // Reset ping-pong and source freq buffers to UAV for next channel.
        ctx.TransitionResource(m_pingpong[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.TransitionResource(m_pingpong[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.TransitionResource(*ch.srcFreq,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Build the gradient-map mip chain (leaves gradMap all-NPSR) for anti-aliased distant normals.
    GenerateGradientMips(ctx, alloc, cs);

    // ---- Transition final maps to NPSR for cross-queue handoff ----
    // PIXEL_SHADER_RESOURCE is invalid on compute queues; Render() adds the PSR bit
    // on the graphics queue before drawing. (gradMap is already NPSR from the mip pass.)
    ctx.TransitionResource(cs.heightMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.TransitionResource(cs.gradMap,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.TransitionResource(cs.dispMap,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// Box-downsample gradMap mip 0 → 1 → … using per-subresource barriers: each source mip is moved to
// NPSR and read via the all-mip SRV (.mips[SrcMip]) while the next mip is written as a UAV. Leaves
// every subresource in NON_PIXEL_SHADER_RESOURCE.
void FFTWaterSurface::GenerateGradientMips(CommandContext& ctx, LinearAllocator& alloc, Cascade& cs)
{
    if (m_gradMips <= 1) return;

    auto* cmd = ctx.GetCommandList();
    const int c = (int)(&cs - &m_cascades[0]);
    ID3D12Resource* res = cs.gradMap.GetResource();
    const UINT srvSlot = (UINT)(kRenderTable + 1 * kNumCascades + c);  // all-mip gradient SRV

    ctx.FlushResourceBarriers();  // drain any batched barriers before raw per-subresource ones
    cmd->SetComputeRootSignature(m_downsampleRootSig.Get());
    ctx.SetPipelineState(DownsamplePSO.Get());

    struct DSParams { uint32_t SrcMip, DstW, DstH, Pad; };

    for (int m = 1; m < m_gradMips; ++m)
    {
        // Source mip (m-1): UAV → NPSR so it can be read via the SRV.
        D3D12_RESOURCE_BARRIER toRead = {};
        toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource   = res;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toRead.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toRead.Transition.Subresource = (UINT)(m - 1);
        cmd->ResourceBarrier(1, &toRead);

        UINT dstW = (UINT)std::max(1, m_desc.N >> m);
        UINT dstH = (UINT)std::max(1, m_desc.M >> m);

        auto cb = alloc.Allocate(sizeof(DSParams));
        DSParams p = { (uint32_t)(m - 1), dstW, dstH, 0 };
        memcpy(cb.cpuAddress, &p, sizeof(p));

        cmd->SetComputeRootConstantBufferView(0, cb.gpuAddress);
        cmd->SetComputeRootDescriptorTable(1, GpuHandle(srvSlot));                          // src (all mips)
        cmd->SetComputeRootDescriptorTable(2, GpuHandle((UINT)(kMipGenBegin + c * kMaxGradMips + m))); // dst mip m
        cmd->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

        // Ensure this mip's write completes before it's read as the next source.
        D3D12_RESOURCE_BARRIER uavB = {};
        uavB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavB.UAV.pResource = res;
        cmd->ResourceBarrier(1, &uavB);
    }

    // Final mip is still UAV — move it to NPSR so the whole resource is uniform again.
    D3D12_RESOURCE_BARRIER last = {};
    last.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    last.Transition.pResource   = res;
    last.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    last.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    last.Transition.Subresource = (UINT)(m_gradMips - 1);
    cmd->ResourceBarrier(1, &last);

    cs.gradMap.SetCurrentState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// ---------------------------------------------------------------------------
// Debug visualization: render each cascade's time-evolved spectrum + heightfield into small
// RGBA textures the UI samples. Runs on the graphics queue (compute dispatch on the direct list),
// so the display textures cycle PSR→UAV→PSR entirely on one queue — no cross-queue handoff.
// ---------------------------------------------------------------------------
void FFTWaterSurface::RenderViz(GraphicsContext& ctx, LinearAllocator& alloc)
{
    auto* cmd = ctx.GetCommandList();
    const int  N      = m_desc.N;
    const UINT groups = (UINT)((N + 7) / 8);

    // Sources readable, destinations writable.
    for (int c = 0; c < kNumCascades; ++c)
    {
        // heightFreq still holds h(k,t) (IFFT read it, didn't overwrite). Reclaimed to UAV next
        // frame by DynamicSpectrum. heightMap is already NPSR/PSR-readable from Update/Render.
        ctx.TransitionResource(m_cascades[c].heightFreq,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.TransitionResource(m_cascades[c].vizSpectrum, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.TransitionResource(m_cascades[c].vizHeight,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ctx.FlushResourceBarriers();

    ctx.SetDescriptorHeap(m_heap.Get());
    cmd->SetComputeRootSignature(m_downsampleRootSig.Get());
    ctx.SetPipelineState(VizPSO.Get());

    struct VizParams { uint32_t Mode, N; float Scale; uint32_t Pad; };

    for (int c = 0; c < kNumCascades; ++c)
    {
        const Cascade& cs = m_cascades[c];

        // Time-evolved spectrum: src = heightFreq SRV (initHeightTable slot 1), dst = vizSpectrum.
        {
            auto cb = alloc.Allocate(sizeof(VizParams));
            VizParams p = { 0u, (uint32_t)N, tweaks.spectrumVizGain, 0u };
            memcpy(cb.cpuAddress, &p, sizeof(p));
            cmd->SetComputeRootConstantBufferView(0, cb.gpuAddress);
            cmd->SetComputeRootDescriptorTable(1, GpuHandle(cs.initHeightTable + 1));
            cmd->SetComputeRootDescriptorTable(2, GpuHandle((UINT)(kVizBegin + c * 2 + 0)));
            cmd->Dispatch(groups, groups, 1);
        }
        // Heightfield: src = heightMap render SRV, dst = vizHeight.
        {
            auto cb = alloc.Allocate(sizeof(VizParams));
            VizParams p = { 1u, (uint32_t)N, tweaks.heightVizGain, 0u };
            memcpy(cb.cpuAddress, &p, sizeof(p));
            cmd->SetComputeRootConstantBufferView(0, cb.gpuAddress);
            cmd->SetComputeRootDescriptorTable(1, GpuHandle((UINT)(kRenderTable + 0 * kNumCascades + c)));
            cmd->SetComputeRootDescriptorTable(2, GpuHandle((UINT)(kVizBegin + c * 2 + 1)));
            cmd->Dispatch(groups, groups, 1);
        }
    }

    // Promote the images to PSR so ImGui can sample them later this frame.
    for (int c = 0; c < kNumCascades; ++c)
    {
        ctx.UAVBarrier(m_cascades[c].vizSpectrum);
        ctx.UAVBarrier(m_cascades[c].vizHeight);
        ctx.TransitionResource(m_cascades[c].vizSpectrum, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ctx.TransitionResource(m_cascades[c].vizHeight,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ctx.FlushResourceBarriers();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void FFTWaterSurface::Render(GraphicsContext& ctx, LinearAllocator& alloc, const View& view,
                             const XMFLOAT3& lightDir,
                             D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSRV,
                             D3D12_CPU_DESCRIPTOR_HANDLE sceneColorSRV,
                             D3D12_CPU_DESCRIPTOR_HANDLE shadowArraySRV,
                             D3D12_GPU_VIRTUAL_ADDRESS   shadowCascadeCB)
{
    // Promote displacement maps from NPSR (written on compute queue) to PSR|NPSR
    // so both the domain shader and pixel shader can read them.
    const D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (int c = 0; c < kNumCascades; ++c)
    {
        ctx.TransitionResource(m_cascades[c].heightMap, kSrvAll);
        ctx.TransitionResource(m_cascades[c].gradMap,   kSrvAll);
        ctx.TransitionResource(m_cascades[c].dispMap,   kSrvAll);
    }
    ctx.FlushResourceBarriers();

    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderSceneDepth), sceneDepthSRV,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderSceneColor), sceneColorSRV,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderShadow), shadowArraySRV,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ctx.SetDescriptorHeap(m_heap.Get());
    ctx.SetRootSignature(m_renderRootSig.Get());

    if (tweaks.tessellation)
        ctx.SetPipelineState(tweaks.wireframe ? WaterWireframePSO.Get() : WaterRenderPSO.Get());
    else
        ctx.SetPipelineState(tweaks.wireframe ? WaterFlatWireframePSO.Get() : WaterFlatPSO.Get());

    auto frameAlloc = alloc.Allocate(sizeof(WaterFrameConstants));
    WaterFrameConstants fc = {};
    fc.CameraPosition = view.position;
    fc.NearZ          = view.nearZ;
    fc.LightDirection = lightDir;
    fc.FarZ           = view.farZ;
    memcpy(frameAlloc.cpuAddress, &fc, sizeof(fc));
    ctx.SetConstantBufferView(0, frameAlloc.gpuAddress);

    // Build a template WaterConstants with fields that are the same for every tile.
    WaterConstants wcBase = {};
    wcBase.WorldViewProjMat = view.viewProjMatrix; // plain ViewProj; WorldMat carries tile offset
    wcBase.ColorMax = XMFLOAT4(tweaks.color.x, tweaks.color.y, tweaks.color.z,
                               tweaks.maxTessellation);
    // CascTess.xyz = 1/tileSize per cascade (drives sample UV + amplitude scale); w = tess distance.
    // A disabled cascade uses rcp = 0, which zeroes its displacement/height/gradient contribution.
    auto rcpOf = [&](int c) {
        return (c < kNumCascades && tweaks.cascadeEnabled[c]) ? 1.0f / m_cascades[c].tileSize : 0.0f;
    };
    wcBase.CascTess = XMFLOAT4(rcpOf(0), rcpOf(1), rcpOf(2), tweaks.tessDistance);
    wcBase.Extra    = XMFLOAT4(tweaks.ambient, 0.0f, 0.0f, 0.0f);

    ctx.SetDescriptorTable(2, GpuHandle(kRenderTable));
    ctx.SetDescriptorTable(3, GpuHandle(kRenderSceneDepth));
    ctx.SetConstantBufferView(4, shadowCascadeCB);   // shadow cascade matrices (b2)

    ctx.SetVertexBuffer(m_meshVertex.GetView());
    if (tweaks.tessellation)
    {
        ctx.SetIndexBuffer(m_meshIndex.GetView());
        ctx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    }
    else
    {
        ctx.SetIndexBuffer(m_meshIndexFlat.GetView());
        ctx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // Tiles are laid out symmetrically around the origin.
    // tileCount=1 → single tile; tileCount=3 → 3×3 grid, etc.
    int half = tweaks.tileCount / 2;
    for (int tz = -half; tz <= half; ++tz)
    {
        for (int tx = -half; tx <= half; ++tx)
        {
            WaterConstants wc = wcBase;
            XMMATRIX worldMat = XMMatrixTranslation(
                tx * m_desc.TileSize, 0.0f, tz * m_desc.TileSize);
            XMStoreFloat4x4(&wc.WorldMat, XMMatrixTranspose(worldMat));

            auto waterAlloc = alloc.Allocate(sizeof(WaterConstants));
            memcpy(waterAlloc.cpuAddress, &wc, sizeof(wc));
            ctx.SetConstantBufferView(1, waterAlloc.gpuAddress);

            ctx.DrawIndexedInstanced(
                tweaks.tessellation ? m_indexCount : m_indexCountFlat, 1, 0, 0, 0);
        }
    }
}
