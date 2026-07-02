#include "Atmosphere.h"
#include "util/Assert.h"
#include <fstream>
#include <vector>
#include <DirectXMath.h>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> ReadShaderBlob(const char* name)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    std::string path = dir + "shaders\\" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = (size_t)f.tellg();
    std::vector<uint8_t> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static ComPtr<ID3D12RootSignature> LoadRootSig(ID3D12Device* device, const char* name)
{
    auto blob = ReadShaderBlob(name);
    ComPtr<ID3D12RootSignature> rs;
    ASSERT_SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(),
                                                  IID_PPV_ARGS(&rs)));
    return rs;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void Atmosphere::Init(ID3D12Device* device)
{
    m_device = device;

    m_computeRootSig = LoadRootSig(device, "atm_compute_rs.cso");
    m_renderRootSig  = LoadRootSig(device, "atm_render_rs.cso");
    m_envCubeRootSig = LoadRootSig(device, "atm_envcube_rs.cso");

    CreateTextures();
    CreateDescriptorHeap();
    BuildDescriptors();
    CreatePSOs();
}

// ---------------------------------------------------------------------------
// PSO creation
// ---------------------------------------------------------------------------

void Atmosphere::CreatePSOs()
{
    auto transBlob   = ReadShaderBlob("atm_transmittance_cs.cso");
    auto skyViewBlob = ReadShaderBlob("atm_skyview_cs.cso");
    auto vsBlob      = ReadShaderBlob("atm_sky_render_vs.cso");
    auto psBlob      = ReadShaderBlob("atm_sky_render_ps.cso");

    // Transmittance LUT compute PSO
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_computeRootSig.Get();
        desc.CS             = { transBlob.data(), transBlob.size() };
        ASSERT_SUCCEEDED(m_device->CreateComputePipelineState(&desc,
                                                               IID_PPV_ARGS(&m_transmittancePSO)));
    }

    // SkyView LUT compute PSO
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_computeRootSig.Get();
        desc.CS             = { skyViewBlob.data(), skyViewBlob.size() };
        ASSERT_SUCCEEDED(m_device->CreateComputePipelineState(&desc,
                                                               IID_PPV_ARGS(&m_skyViewPSO)));
    }

    // Env cubemap compute PSO
    {
        auto envCubeBlob = ReadShaderBlob("atm_envcube_cs.cso");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_envCubeRootSig.Get();
        desc.CS             = { envCubeBlob.data(), envCubeBlob.size() };
        ASSERT_SUCCEEDED(m_device->CreateComputePipelineState(&desc,
                                                               IID_PPV_ARGS(&m_envCubePSO)));
    }

    // Sky render graphics PSO
    {
        D3D12_RASTERIZER_DESC raster = {};
        raster.FillMode        = D3D12_FILL_MODE_SOLID;
        raster.CullMode        = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable = FALSE; // sky is always drawn as background; depth test disabled

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature        = m_renderRootSig.Get();
        desc.VS                    = { vsBlob.data(), vsBlob.size() };
        desc.PS                    = { psBlob.data(), psBlob.size() };
        desc.RasterizerState       = raster;
        desc.BlendState            = blend;
        desc.DepthStencilState     = ds;
        desc.SampleMask            = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets      = 1;
        desc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count      = 1;

        ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&desc,
                                                                IID_PPV_ARGS(&m_skyRenderPSO)));
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

void Atmosphere::CreateTextures()
{
    m_transmittanceLUT.Create(m_device, DXGI_FORMAT_R16G16B16A16_FLOAT, 256, 64,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_transmittanceLUT.SetDebugName(L"Atmosphere/TransmittanceLUT");

    m_skyViewLUT.Create(m_device, DXGI_FORMAT_R16G16B16A16_FLOAT, 512, 256,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_skyViewLUT.SetDebugName(L"Atmosphere/SkyViewLUT");

    // Env cubemap: 128×128×6 RGBA16F, writable as Texture2DArray UAV,
    // readable as TextureCube SRV by the water pixel shader.
    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC cubeDesc = {};
    cubeDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cubeDesc.Width              = kEnvCubeSize;
    cubeDesc.Height             = kEnvCubeSize;
    cubeDesc.DepthOrArraySize   = 6;
    cubeDesc.MipLevels          = 1;
    cubeDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cubeDesc.SampleDesc.Count   = 1;
    cubeDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ASSERT_SUCCEEDED(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &cubeDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_envCubeMap)));
    m_envCubeMap->SetName(L"Atmosphere/EnvCubeMap");
}

// ---------------------------------------------------------------------------
// Descriptor heap
// ---------------------------------------------------------------------------

void Atmosphere::CreateDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = kHeapSize;
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ASSERT_SUCCEEDED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)));
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

D3D12_GPU_DESCRIPTOR_HANDLE Atmosphere::GpuHandle(UINT slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot * m_descriptorSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Atmosphere::CpuHandle(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * m_descriptorSize;
    return h;
}

void Atmosphere::BuildDescriptors()
{
    // Null SRV (for passes that don't use t0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
        nullDesc.Format                        = DXGI_FORMAT_R32G32B32A32_FLOAT;
        nullDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels           = 1;
        m_device->CreateShaderResourceView(nullptr, &nullDesc, CpuHandle(kNullSRV));
    }

    auto MakeSRV = [&](UINT slot, PixelBuffer& buf) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format                    = buf.GetFormat();
        d.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(buf.GetResource(), &d, CpuHandle(slot));
    };

    auto MakeUAV = [&](UINT slot, PixelBuffer& buf) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format        = buf.GetFormat();
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(buf.GetResource(), nullptr, &d, CpuHandle(slot));
    };

    MakeSRV(kTransmitSRV,      m_transmittanceLUT);
    MakeSRV(kSkyViewSRV,       m_skyViewLUT);
    MakeUAV(kTransmitUAV,      m_transmittanceLUT);
    MakeUAV(kSkyViewUAV,       m_skyViewLUT);
    // Render-table copies: create directly rather than CopyDescriptorsSimple from shader-visible heap.
    MakeSRV(kRenderSkyViewSRV,  m_skyViewLUT);
    MakeSRV(kRenderTransmitSRV, m_transmittanceLUT);

    // Env cubemap UAV: all 6 array slices, written by the cubemap compute shader.
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice        = 0;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize       = 6;
        m_device->CreateUnorderedAccessView(m_envCubeMap.Get(), nullptr, &d, CpuHandle(kEnvCubeUAV));
    }
}

// ---------------------------------------------------------------------------
// GpuParams helper
// ---------------------------------------------------------------------------

Atmosphere::GpuAtmParams Atmosphere::BuildGpuParams() const
{
    GpuAtmParams p;
    p.RayleighBeta        = params.rayleighBeta;
    p.RayleighScaleHeight = params.rayleighScaleHeight;
    p.MieBeta             = params.mieBeta;
    p.MieScaleHeight      = params.mieScaleHeight;
    p.MieBetaExt          = params.mieBetaExt;
    p.MieG                = params.mieG;
    p.OzoneBetaAbs        = params.ozoneBetaAbs;
    p.OzoneCenterHeight   = params.ozoneCenterHeight;
    p.OzoneWidth          = params.ozoneWidth;
    p.BottomRadius        = params.bottomRadius;
    p.TopRadius           = params.topRadius;
    p.SunIntensity        = params.sunIntensity;
    return p;
}

// ---------------------------------------------------------------------------
// Update — compute passes
// ---------------------------------------------------------------------------

void Atmosphere::Update(CommandContext& ctx, LinearAllocator& alloc,
                        const XMFLOAT3& sunDir, float cameraAltitudeM)
{
    auto* cmd = ctx.GetCommandList();

    ctx.SetDescriptorHeap(m_heap.Get());

    GpuAtmParams atmParams = BuildGpuParams();
    auto atmAlloc = alloc.Allocate(256);
    memcpy(atmAlloc.cpuAddress, &atmParams, sizeof(atmParams));

    // ---- TransmittanceLUT (recomputed when dirty) ----
    if (m_transmittanceDirty)
    {
        ctx.TransitionResource(m_transmittanceLUT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.FlushResourceBarriers();

        struct Dummy { float _pad[4]; };
        auto dummyAlloc = alloc.Allocate(256);

        cmd->SetComputeRootSignature(m_computeRootSig.Get());
        cmd->SetPipelineState(m_transmittancePSO.Get());
        cmd->SetComputeRootConstantBufferView(0, atmAlloc.gpuAddress);
        cmd->SetComputeRootConstantBufferView(1, dummyAlloc.gpuAddress);
        ctx.FlushResourceBarriers();
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(kNullSRV));
        cmd->SetComputeRootDescriptorTable(3, GpuHandle(kTransmitUAV));
        cmd->Dispatch((256 + 7) / 8, (64 + 7) / 8, 1);

        ctx.UAVBarrier(m_transmittanceLUT);
        m_transmittanceDirty = false;
    }

    // NON_PIXEL_SHADER_RESOURCE only — compute queues do not support PIXEL_SHADER_RESOURCE.
    // Render() promotes both LUTs to PSR|NPSR before the sky draw.
    const D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Transmittance LUT must be SRV-readable for the SkyView CS.
    ctx.TransitionResource(m_transmittanceLUT, kSrv);
    ctx.TransitionResource(m_skyViewLUT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.FlushResourceBarriers();

    // ---- SkyViewLUT (every frame) ----
    {
        float altKm = cameraAltitudeM / 1000.0f;
        float r     = params.bottomRadius + max(0.0f, altKm);

        GpuSkyViewConstants sv;
        sv.CameraPositionAtm = XMFLOAT3(0.0f, r, 0.0f);
        sv._pad0             = 0.0f;
        sv.SunDirection      = sunDir;
        sv._pad1             = 0.0f;

        m_lastSunDir = sunDir;

        auto svAlloc = alloc.Allocate(256);
        memcpy(svAlloc.cpuAddress, &sv, sizeof(sv));

        cmd->SetComputeRootSignature(m_computeRootSig.Get());
        cmd->SetPipelineState(m_skyViewPSO.Get());
        cmd->SetComputeRootConstantBufferView(0, atmAlloc.gpuAddress);
        cmd->SetComputeRootConstantBufferView(1, svAlloc.gpuAddress);
        ctx.FlushResourceBarriers();
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(kTransmitSRV));
        cmd->SetComputeRootDescriptorTable(3, GpuHandle(kSkyViewUAV));
        cmd->Dispatch((512 + 7) / 8, (256 + 7) / 8, 1);

        ctx.UAVBarrier(m_skyViewLUT);
    }

    // SkyViewLUT → pixel-shader readable for Render(). Transmittance is already in kSrv.
    ctx.TransitionResource(m_skyViewLUT, kSrv);
    ctx.FlushResourceBarriers();

    // ---- Env cubemap (every frame, samples the freshly-computed SkyViewLUT) ----
    {
        float altKm = max(0.0f, cameraAltitudeM / 1000.0f);
        float r     = params.bottomRadius + altKm;

        // Transition cubemap to UAV for writing (PSR on all frames after the first).
        if (m_envCubeState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER toUav = {};
            toUav.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toUav.Transition.pResource        = m_envCubeMap.Get();
            toUav.Transition.StateBefore      = m_envCubeState;
            toUav.Transition.StateAfter       = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            toUav.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &toUav);
            m_envCubeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        GpuSkyRenderConstants rc;
        XMStoreFloat4x4(&rc.InvViewProj, XMMatrixIdentity());  // unused by cubemap shader
        rc.CameraPositionAtm = XMFLOAT3(0.0f, r, 0.0f);
        rc._pad0             = 0.0f;
        rc.SunDirection      = sunDir;
        rc.SunIntensity      = params.sunIntensity;
        rc.BottomRadius      = params.bottomRadius;
        rc.TopRadius         = params.topRadius;
        rc._pad1[0] = rc._pad1[1] = 0.0f;

        auto rcAlloc = alloc.Allocate(sizeof(GpuSkyRenderConstants));
        memcpy(rcAlloc.cpuAddress, &rc, sizeof(rc));

        // FaceIndices: maps each dispatch z-group to a D3D12 cubemap face index.
        // Only the upper hemisphere is dispatched — lower hemisphere stays black (never
        // sampled by water reflections since the surface normal always points upward).
        struct CubeDispatch { uint32_t FaceIndices[4]; };

        cmd->SetComputeRootSignature(m_envCubeRootSig.Get());
        cmd->SetPipelineState(m_envCubePSO.Get());
        cmd->SetComputeRootConstantBufferView(0, rcAlloc.gpuAddress);
        ctx.FlushResourceBarriers();
        // kRenderSkyViewSRV(5) and kRenderTransmitSRV(6) are consecutive:
        //   t0 = SkyViewLUT,  t1 = TransmittanceLUT — matches cubemap shader registers.
        cmd->SetComputeRootDescriptorTable(2, GpuHandle(kRenderSkyViewSRV));
        cmd->SetComputeRootDescriptorTable(3, GpuHandle(kEnvCubeUAV));

        // Dispatch 1: face +Y (index 2), all rows — the +Y face is entirely sky.
        {
            CubeDispatch cd = { {2, 0, 0, 0} };
            auto b1 = alloc.Allocate(256);
            memcpy(b1.cpuAddress, &cd, sizeof(cd));
            cmd->SetComputeRootConstantBufferView(1, b1.gpuAddress);
            cmd->Dispatch(kEnvCubeSize / 8, kEnvCubeSize / 8, 1);
        }

        // UAV barrier between the two dispatches.
        {
            D3D12_RESOURCE_BARRIER uavB = {};
            uavB.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavB.UAV.pResource = m_envCubeMap.Get();
            cmd->ResourceBarrier(1, &uavB);
        }

        // Dispatch 2: 4 side faces (+X=0, -X=1, +Z=4, -Z=5), top half only.
        // The top half (rows 0..kEnvCubeSize/2-1) maps to UV.y > 0 after the Y-flip,
        // which corresponds to ray directions with a positive Y component — all sky.
        {
            CubeDispatch cd = { {0, 1, 4, 5} };
            auto b1 = alloc.Allocate(256);
            memcpy(b1.cpuAddress, &cd, sizeof(cd));
            cmd->SetComputeRootConstantBufferView(1, b1.gpuAddress);
            cmd->Dispatch(kEnvCubeSize / 8, kEnvCubeSize / 16, 4);
        }

        // UAV barrier then transition to NPSR — compute queues cannot use PSR.
        // Render() promotes to PSR on the graphics queue before the water draw.
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = m_envCubeMap.Get();
        barriers[1].Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource        = m_envCubeMap.Get();
        barriers[1].Transition.StateBefore      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter       = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(2, barriers);
        m_envCubeState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
}

// ---------------------------------------------------------------------------
// Render — fullscreen sky pass
// ---------------------------------------------------------------------------

void Atmosphere::PrepareForCompute(CommandContext& ctx)
{
    const D3D12_RESOURCE_STATES kNpsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // TransitionResource is a no-op if already in NPSR, so this is always safe to call.
    ctx.TransitionResource(m_skyViewLUT,       kNpsr);
    ctx.TransitionResource(m_transmittanceLUT, kNpsr);
    if (m_envCubeState != kNpsr)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource        = m_envCubeMap.Get();
        b.Transition.StateBefore      = m_envCubeState;
        b.Transition.StateAfter       = kNpsr;
        b.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx.GetCommandList()->ResourceBarrier(1, &b);
        m_envCubeState = kNpsr;
    }
    ctx.FlushResourceBarriers();
}

void Atmosphere::Render(GraphicsContext& ctx, LinearAllocator& alloc, const View& view)
{
    auto* cmd = ctx.GetCommandList();

    // Promote LUTs from NPSR (written on compute queue) to PSR|NPSR for pixel shader reads.
    const D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ctx.TransitionResource(m_skyViewLUT,       kSrvAll);
    ctx.TransitionResource(m_transmittanceLUT, kSrvAll);

    // Promote envCubeMap from NPSR to PSR so the water shader can sample it after this pass.
    if (m_envCubeState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource        = m_envCubeMap.Get();
        b.Transition.StateBefore      = m_envCubeState;
        b.Transition.StateAfter       = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_envCubeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    ctx.FlushResourceBarriers();

    // Build inverse view-proj
    XMMATRIX vp = XMLoadFloat4x4(&view.viewProjMatrix);
    XMMATRIX invVP;
    // view.viewProjMatrix is stored transposed (column-major for HLSL).
    // Transpose back to row-major for XMMatrixInverse, then transpose again for the shader.
    XMMATRIX vpRowMajor = XMMatrixTranspose(vp);
    XMVECTOR det;
    invVP = XMMatrixTranspose(XMMatrixInverse(&det, vpRowMajor));

    float altKm = max(0.0f, view.position.y / 1000.0f);
    float r     = params.bottomRadius + altKm;

    GpuSkyRenderConstants rc;
    XMStoreFloat4x4(&rc.InvViewProj, invVP);
    rc.CameraPositionAtm = XMFLOAT3(0.0f, r, 0.0f);
    rc._pad0             = 0.0f;
    rc.SunDirection      = m_lastSunDir;
    rc.SunIntensity      = params.sunIntensity;
    rc.BottomRadius      = params.bottomRadius;
    rc.TopRadius         = params.topRadius;
    rc._pad1[0] = rc._pad1[1] = 0.0f;

    auto rcAlloc = alloc.Allocate(sizeof(GpuSkyRenderConstants));
    memcpy(rcAlloc.cpuAddress, &rc, sizeof(rc));

    ctx.SetDescriptorHeap(m_heap.Get());
    cmd->SetGraphicsRootSignature(m_renderRootSig.Get());
    cmd->SetPipelineState(m_skyRenderPSO.Get());
    cmd->SetGraphicsRootConstantBufferView(0, rcAlloc.gpuAddress);
    cmd->SetGraphicsRootDescriptorTable(1, GpuHandle(kRenderSkyViewSRV));

    ctx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0); // fullscreen triangle
}
