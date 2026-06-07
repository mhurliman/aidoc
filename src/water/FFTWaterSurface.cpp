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

void FFTWaterSurface::Init(ID3D12Device* device, const WaterDesc& desc)
{
    m_device = device;
    m_desc   = desc;

    int n = desc.N;
    m_log2N = 0;
    while ((1 << m_log2N) < n)
        ++m_log2N;

    m_computeRootSig = LoadRootSig(m_device, "fft_compute_rs.cso");
    m_renderRootSig  = LoadRootSig(m_device, "water_render_rs.cso");
    CreatePSOs();
    CreateTextures();
    CreateDescriptorHeap();
    BuildDescriptorTables();
    tweaks.meshResolution = desc.MeshResolution;
    CreateMesh();
    m_lastMeshResolution = desc.MeshResolution;
    GenerateNoise();
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

    // Noise: SRV-only, data arrives via CopyTextureRegion on first Update.
    m_noise.Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N,
                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    m_h0.Create        (m_device, DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, kUAV, kUAVState);
    m_heightFreq.Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_gradFreq.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_dispFreq.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_pingpong[0].Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);
    m_pingpong[1].Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, N, kUAV, kUAVState);

    // Twiddle factor table: log2N columns, N rows.
    m_precomputed.Create(m_device, DXGI_FORMAT_R32G32B32A32_FLOAT, m_log2N, N, kUAV, kUAVState);

    m_heightMap.Create(m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState);
    m_gradMap.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState);
    m_dispMap.Create  (m_device, DXGI_FORMAT_R32G32_FLOAT, N, M, kUAV, kUAVState);
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
    // Individual resource descriptors
    CreateSRV2D(m_device, m_noise,       CpuHandle(kNoiseSRV));
    CreateSRV2D(m_device, m_h0,          CpuHandle(kH0SRV));
    CreateSRV2D(m_device, m_heightFreq,  CpuHandle(kHeightFreqSRV));
    CreateSRV2D(m_device, m_gradFreq,    CpuHandle(kGradFreqSRV));
    CreateSRV2D(m_device, m_dispFreq,    CpuHandle(kDispFreqSRV));
    CreateSRV2D(m_device, m_pingpong[0], CpuHandle(kPP0SRV));
    CreateSRV2D(m_device, m_pingpong[1], CpuHandle(kPP1SRV));
    CreateSRV2D(m_device, m_precomputed, CpuHandle(kPrecompSRV));
    CreateSRV2D(m_device, m_heightMap,   CpuHandle(kHeightMapSRV));
    CreateSRV2D(m_device, m_gradMap,     CpuHandle(kGradMapSRV));
    CreateSRV2D(m_device, m_dispMap,     CpuHandle(kDispMapSRV));
    CreateNullSRV(m_device, CpuHandle(kNullSRV));

    CreateNullUAV(m_device, CpuHandle(kNullUAV));
    CreateUAV2D(m_device, m_h0,          CpuHandle(kH0UAV));
    CreateUAV2D(m_device, m_heightFreq,  CpuHandle(kHeightFreqUAV));
    CreateUAV2D(m_device, m_gradFreq,    CpuHandle(kGradFreqUAV));
    CreateUAV2D(m_device, m_dispFreq,    CpuHandle(kDispFreqUAV));
    CreateUAV2D(m_device, m_pingpong[0], CpuHandle(kPP0UAV));
    CreateUAV2D(m_device, m_pingpong[1], CpuHandle(kPP1UAV));
    CreateUAV2D(m_device, m_precomputed, CpuHandle(kPrecompUAV));
    CreateUAV2D(m_device, m_heightMap,   CpuHandle(kHeightMapUAV));
    CreateUAV2D(m_device, m_gradMap,     CpuHandle(kGradMapUAV));
    CreateUAV2D(m_device, m_dispMap,     CpuHandle(kDispMapUAV));

    auto fillComputeTable = [&](UINT base,
                                 UINT s0, UINT s1, UINT s2,
                                 UINT u0, UINT u1, UINT u2)
    {
        UINT srvs[] = { s0, s1, s2 };
        UINT uavs[] = { u0, u1, u2 };
        for (UINT i = 0; i < 3; ++i)
            m_device->CopyDescriptorsSimple(1, CpuHandle(base + i),
                                            CpuHandle(srvs[i]),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = 0; i < 3; ++i)
            m_device->CopyDescriptorsSimple(1, CpuHandle(base + 3 + i),
                                            CpuHandle(uavs[i]),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    };

    fillComputeTable(kPhillipsTable,   kNoiseSRV,      kNullSRV, kNullSRV, kH0UAV,        kNullUAV, kNullUAV);
    fillComputeTable(kDynSpecTable,    kH0SRV,         kNullSRV, kNullSRV, kHeightFreqUAV, kGradFreqUAV, kDispFreqUAV);
    fillComputeTable(kPrecomputeTable, kNullSRV,       kNullSRV, kNullSRV, kPrecompUAV,   kNullUAV, kNullUAV);
    fillComputeTable(kInitHeightTable, kPrecompSRV,    kHeightFreqSRV, kNullSRV, kPP0UAV, kNullUAV, kNullUAV);
    fillComputeTable(kInitGradTable,   kPrecompSRV,    kGradFreqSRV,   kNullSRV, kPP0UAV, kNullUAV, kNullUAV);
    fillComputeTable(kInitDispTable,   kPrecompSRV,    kDispFreqSRV,   kNullSRV, kPP0UAV, kNullUAV, kNullUAV);
    fillComputeTable(kIFFT0to1Table,   kPrecompSRV,    kPP0SRV,  kNullSRV, kPP1UAV,       kNullUAV, kNullUAV);
    fillComputeTable(kIFFT1to0Table,   kPrecompSRV,    kPP1SRV,  kNullSRV, kPP0UAV,       kNullUAV, kNullUAV);
    fillComputeTable(kPermuteHTable,   kPP1SRV,        kNullSRV, kNullSRV, kHeightMapUAV, kNullUAV, kNullUAV);
    fillComputeTable(kPermuteGTable,   kPP1SRV,        kNullSRV, kNullSRV, kGradMapUAV,   kNullUAV, kNullUAV);
    fillComputeTable(kPermuteDTable,   kPP1SRV,        kNullSRV, kNullSRV, kDispMapUAV,   kNullUAV, kNullUAV);

    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderTable + 0), CpuHandle(kHeightMapSRV),
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderTable + 1), CpuHandle(kGradMapSRV),
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderTable + 2), CpuHandle(kDispMapSRV),
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CreateNullSRV    (m_device, CpuHandle(kRenderTable + 3));
    CreateNullSRV    (m_device, CpuHandle(kRenderTable + 4));
    CreateNullSRVCube(m_device, CpuHandle(kRenderTable + 5));
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

    UINT64 uploadSize;
    m_device->GetCopyableFootprints(&noiseDesc, 0, 1, 0,
                                    &m_noiseFootprint, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = uploadSize;
    bd.Height           = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ASSERT_SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, IID_PPV_ARGS(&m_noiseUpload)));

    m_noiseCpu.resize((size_t)N * N);

    uint8_t* mapped = nullptr;
    m_noiseUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    srand(12345);
    for (int y = 0; y < N; ++y)
    {
        float* row = reinterpret_cast<float*>(
            mapped + m_noiseFootprint.Offset + (UINT64)y * m_noiseFootprint.Footprint.RowPitch);
        for (int x = 0; x < N; ++x)
        {
            float u1 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
            float u2 = (rand() + 1.0f) / (RAND_MAX + 1.0f);
            float mag = sqrtf(-2.0f * logf(u1));
            float gx = mag * cosf(6.28318530f * u2);
            float gy = mag * sinf(6.28318530f * u2);
            row[x * 2 + 0] = gx;
            row[x * 2 + 1] = gy;
            m_noiseCpu[y * N + x] = { gx, gy };
        }
    }
    m_noiseUpload->Unmap(0, nullptr);
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
                                       CpuHandle(kRenderTable + 5));
}

// ---------------------------------------------------------------------------
// CPU height sampling (low-frequency Tessendorf sum)
// ---------------------------------------------------------------------------

float FFTWaterSurface::SampleHeightCPU(float worldX, float worldZ, float elapsedTime, int modes) const
{
    if (m_noiseCpu.empty())
        return 0.0f;

    const float Pi     = 3.14159265f;
    const float g      = 9.81f;
    const float time   = elapsedTime * tweaks.timeScale + 60.0f;
    const int   N      = m_desc.N;
    const int   halfN  = N / 2;
    const float DeltaK = 2.0f * Pi / m_desc.TileSize;
    const float HalfScale = (1.0f / m_desc.TileSize) * 0.5f;

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
        return tweaks.amplitude * expf(-1.0f / kL2) * (kw * kw) * cutoff / (k2 * k2);
    };

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

            XMFLOAT2 noiseK    = m_noiseCpu[tz  * N + tx];
            XMFLOAT2 noiseMinK = m_noiseCpu[mtz * N + mtx];

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
            // mirrors DynamicSpectrum.hlsl ComplexMult(h0.xy, E) + ComplexMult(h0.zw, conj(E))
            float hRe = (h0Kx*eRe - h0Ky*eIm) + (h0mKx*eRe + h0mKy*eIm);
            float hIm = (h0Kx*eIm + h0Ky*eRe) + (-h0mKx*eIm + h0mKy*eRe);

            // Accumulate real part of h(k,t) * exp(i*k·x)
            float kDotX = kx * worldX + kz * worldZ;
            height += hRe * cosf(kDotX) - hIm * sinf(kDotX);
        }
    }

    return height * HalfScale;
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

    // ---- One-time precomputation on first frame ----
    //if (!m_precomputeDone)
    {
        // Upload noise data then transition to shader-readable state.
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource       = m_noiseUpload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = m_noiseFootprint;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource        = m_noise.GetResource();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        ctx.TransitionResource(m_noise, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Phillips spectrum (once)
        {
            struct PhillipsGlobals { float windTheta, windSpeed, smallWaveCutoff, amplitude; };
            auto fftAlloc  = alloc.Allocate(sizeof(FFTParameters));
            FFTParameters fftParams = { (uint32_t)N, m_desc.TileSize, 1.0f / m_desc.TileSize };
            memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));

            auto globAlloc = alloc.Allocate(sizeof(PhillipsGlobals));
            PhillipsGlobals globals = { tweaks.windTheta, tweaks.windSpeed,
                                        tweaks.smallWaveCutoff, tweaks.amplitude };
            memcpy(globAlloc.cpuAddress, &globals, sizeof(globals));

            cmd->SetComputeRootSignature(m_computeRootSig.Get());
            ctx.SetPipelineState(PhillipsSpectrumPSO.Get());
            cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, globAlloc.gpuAddress);
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(kPhillipsTable));
            ctx.FlushResourceBarriers();
            cmd->Dispatch(groupsN, groupsN, 1);

            ctx.UAVBarrier(m_h0);
            // H0 stays NON_PIXEL_SHADER_RESOURCE permanently after this.
            ctx.TransitionResource(m_h0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Precompute twiddle factors (once)
        {
            auto fftAlloc   = alloc.Allocate(sizeof(FFTParameters));
            FFTParameters fftParams = { (uint32_t)N, m_desc.TileSize, 1.0f / m_desc.TileSize };
            memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));
            auto dummyAlloc = alloc.Allocate(256);

            ctx.SetPipelineState(PrecomputePSO.Get());
            cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
            cmd->SetComputeRootConstantBufferView(1, dummyAlloc.gpuAddress);
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(kPrecomputeTable));
            ctx.FlushResourceBarriers();
            cmd->Dispatch((UINT)m_log2N, groupsN / 2, 1);

            ctx.UAVBarrier(m_precomputed);
            ctx.TransitionResource(m_precomputed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        m_precomputeDone = true;
    }

    // ---- Transition final maps back to UAV for this frame's writes ----
    // TransitionResource is a no-op if state is already UNORDERED_ACCESS (first frame),
    // so no separate firstFrame flag is needed.
    ctx.TransitionResource(m_heightMap, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.TransitionResource(m_gradMap,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.TransitionResource(m_dispMap,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- Dynamic spectrum (per-frame) ----
    {
        auto fftAlloc  = alloc.Allocate(sizeof(FFTParameters));
        FFTParameters fftParams = { (uint32_t)N, m_desc.TileSize, 1.0f / m_desc.TileSize };
        memcpy(fftAlloc.cpuAddress, &fftParams, sizeof(fftParams));

        auto timeAlloc = alloc.Allocate(256);
        time = time * tweaks.timeScale + 60.0f;
        struct DynSpecGlobals { float SimulationTime; float Choppiness; };
        DynSpecGlobals dynGlobals = { time, tweaks.choppiness };
        memcpy(timeAlloc.cpuAddress, &dynGlobals, sizeof(dynGlobals));

        cmd->SetComputeRootSignature(m_computeRootSig.Get());
        ctx.SetPipelineState(DynamicSpectrumPSO.Get());
        cmd->SetComputeRootConstantBufferView(0, fftAlloc.gpuAddress);
        cmd->SetComputeRootConstantBufferView(1, timeAlloc.gpuAddress);
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(kDynSpecTable));
        ctx.FlushResourceBarriers();
        cmd->Dispatch(groupsN, groupsN, 1);
    }

    ctx.UAVBarrier(m_heightFreq);
    ctx.UAVBarrier(m_gradFreq);
    ctx.UAVBarrier(m_dispFreq);

    // ---- IFFT + Permute for each channel ----
    struct ChannelDesc { UINT initTable; PixelBuffer* srcFreq; UINT permuteTable; PixelBuffer* finalMap; };
    ChannelDesc channels[3] = {
        { kInitHeightTable, &m_heightFreq, kPermuteHTable, &m_heightMap },
        { kInitGradTable,   &m_gradFreq,   kPermuteGTable, &m_gradMap   },
        { kInitDispTable,   &m_dispFreq,   kPermuteDTable, &m_dispMap   },
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
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(tableBase));
            ctx.FlushResourceBarriers();
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
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(tableBase));
            ctx.FlushResourceBarriers();
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
            cmd->SetComputeRootDescriptorTable(2, GpuHandle(ch.permuteTable));
            ctx.FlushResourceBarriers();
            cmd->Dispatch(groupsN, groupsN, 1);
            ctx.UAVBarrier(*ch.finalMap);
        }

        // Reset ping-pong and source freq buffers to UAV for next channel.
        ctx.TransitionResource(m_pingpong[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.TransitionResource(m_pingpong[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.TransitionResource(*ch.srcFreq,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ---- Transition final maps to SRV for Render ----
    // Include NON_PIXEL_SHADER_RESOURCE so the non-tess VS can also read them.
    const D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ctx.TransitionResource(m_heightMap, kSrvAll);
    ctx.TransitionResource(m_gradMap,   kSrvAll);
    ctx.TransitionResource(m_dispMap,   kSrvAll);
    ctx.FlushResourceBarriers();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void FFTWaterSurface::Render(GraphicsContext& ctx, LinearAllocator& alloc, const View& view,
                             const XMFLOAT3& lightDir,
                             D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSRV,
                             D3D12_CPU_DESCRIPTOR_HANDLE sceneColorSRV)
{
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderTable + 3), sceneDepthSRV,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CopyDescriptorsSimple(1, CpuHandle(kRenderTable + 4), sceneColorSRV,
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
    wcBase.Color            = tweaks.color;
    wcBase.TileSize         = m_desc.TileSize;
    wcBase.RcpTileSize      = 1.0f / m_desc.TileSize;
    wcBase.MaxTessellation  = tweaks.maxTessellation;
    wcBase.TessDistance     = tweaks.tessDistance;

    ctx.SetDescriptorTable(2, GpuHandle(kRenderTable));
    ctx.SetDescriptorTable(3, GpuHandle(kRenderTable + 3));

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
