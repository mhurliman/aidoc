#include "FFTWaterSurface.h"
#include "util/Assert.h"

#include <fstream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <DirectXMath.h>
#include <stb_image.h>

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
    CreateMesh();
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

    auto vsBlob = ReadShaderBlob("water_surface_vs.cso");
    auto psBlob = ReadShaderBlob("water_surface_ps.cso");

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
    gpsDesc.PS                    = { psBlob.data(), psBlob.size() };
    gpsDesc.InputLayout           = { inputLayout, _countof(inputLayout) };
    gpsDesc.RasterizerState       = rasterDesc;
    gpsDesc.DepthStencilState     = dsDesc;
    gpsDesc.BlendState            = blendDesc;
    gpsDesc.SampleMask            = UINT_MAX;
    gpsDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpsDesc.NumRenderTargets      = 1;
    gpsDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpsDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    gpsDesc.SampleDesc.Count      = 1;

    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&gpsDesc, IID_PPV_ARGS(&WaterRenderPSO)));
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

void FFTWaterSurface::CreateMesh()
{
    int N = m_desc.N;
    int M = m_desc.M;

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

    std::vector<uint32_t> indices;
    indices.reserve((size_t)(N - 1) * (M - 1) * 6);
    for (int row = 0; row < M - 1; ++row)
        for (int col = 0; col < N - 1; ++col)
        {
            uint32_t tl = row * N + col;
            indices.push_back(tl);     indices.push_back(tl + N); indices.push_back(tl + 1);
            indices.push_back(tl + 1); indices.push_back(tl + N); indices.push_back(tl + N + 1);
        }

    m_indexCount = static_cast<uint32_t>(indices.size());

    m_meshVertex.CreateAndUpload(m_device, verts.data(),
                                  verts.size() * sizeof(Vertex), sizeof(Vertex));
    m_meshIndex.CreateAndUpload(m_device, indices.data(),
                                 indices.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
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
            row[x * 2 + 0] = mag * cosf(6.28318530f * u2);
            row[x * 2 + 1] = mag * sinf(6.28318530f * u2);
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
// Update
// ---------------------------------------------------------------------------

void FFTWaterSurface::Update(CommandContext& ctx, LinearAllocator& alloc, float time)
{
    auto* cmd = ctx.GetCommandList();
    int N = m_desc.N;
    UINT groupsN = (UINT)(N / 8);

    ctx.SetDescriptorHeap(m_heap.Get());

    // ---- One-time precomputation on first frame ----
    if (!m_precomputeDone)
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
        time += 60.0f;
        memcpy(timeAlloc.cpuAddress, &time, sizeof(float));

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
    ctx.TransitionResource(m_heightMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.TransitionResource(m_gradMap,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.TransitionResource(m_dispMap,   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
    ctx.SetPipelineState(WaterRenderPSO.Get());

    auto frameAlloc = alloc.Allocate(sizeof(WaterFrameConstants));
    WaterFrameConstants fc = {};
    fc.CameraPosition = view.position;
    fc.NearZ          = view.nearZ;
    fc.LightDirection = lightDir;
    fc.FarZ           = view.farZ;
    memcpy(frameAlloc.cpuAddress, &fc, sizeof(fc));
    ctx.SetConstantBufferView(0, frameAlloc.gpuAddress);

    auto waterAlloc = alloc.Allocate(sizeof(WaterConstants));
    WaterConstants wc = {};
    XMStoreFloat4x4(&wc.WorldMat, XMMatrixTranspose(XMMatrixIdentity()));
    wc.WorldViewProjMat = view.viewProjMatrix;
    wc.Color            = tweaks.color;
    wc.TileSize         = m_desc.TileSize;
    wc.RcpTileSize      = 1.0f / m_desc.TileSize;
    memcpy(waterAlloc.cpuAddress, &wc, sizeof(wc));
    ctx.SetConstantBufferView(1, waterAlloc.gpuAddress);

    ctx.SetDescriptorTable(2, GpuHandle(kRenderTable));
    ctx.SetDescriptorTable(3, GpuHandle(kRenderTable + 3));

    ctx.SetVertexBuffer(m_meshVertex.GetView());
    ctx.SetIndexBuffer(m_meshIndex.GetView());
    ctx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}
