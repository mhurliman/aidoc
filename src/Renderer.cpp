#include "Renderer.h"
#include "gfx/ColorBuffer.h"
#include "gfx/DepthBuffer.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "assets/Texture.h"
#include "scene/Scene.h"
#include "util/Assert.h"
#include <cstring>
#include <fstream>
#include <vector>

static std::vector<uint8_t> ReadBlobFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return {};
    auto size = (size_t)f.tellg();
    std::vector<uint8_t> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static std::string ResolveShaderPath(const char* filename)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos + 1);
    return dir + "shaders\\" + filename;
}

static ComPtr<ID3D12RootSignature> LoadRootSignature(ID3D12Device* device, const char* blobName)
{
    auto blob = ReadBlobFile(ResolveShaderPath(blobName));
    ComPtr<ID3D12RootSignature> rootSig;
    ASSERT_SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(),
                                                  IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

void Renderer::Init(ID3D12Device* device, ID3D12CommandQueue* queue, UINT frameCount)
{
    m_device        = device;
    m_graphicsQueue = queue;
    m_frameCount    = frameCount;

    m_gfxContext.Init(device, queue);

    // Async compute queue — runs atmosphere LUT + water FFT in parallel with graphics.
    D3D12_COMMAND_QUEUE_DESC cqDesc = {};
    cqDesc.Type     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    cqDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ASSERT_SUCCEEDED(device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&m_computeQueue)));

    m_computeAllocators.resize(frameCount);
    for (UINT i = 0; i < frameCount; ++i)
        ASSERT_SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_computeAllocators[i])));

    ASSERT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_computeFence)));
    m_computeContext.Init(device, m_computeQueue.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);

    m_profiler.Init(device, queue, (int)frameCount);

    m_resourceManager.Init(device, queue);
    m_transientHeap.Init(device, 1024, 1);
    m_linearAllocator.Init(device, 256 * 1024, frameCount);

    // Create null SRV for materials without textures
    UINT nullSrvIndex;
    m_nullSrvHandle = m_resourceManager.GetSRVHeap().Allocate(nullSrvIndex);
    
    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &nullDesc, m_nullSrvHandle);

    m_rootSignature = LoadRootSignature(device, "pbr_rs.cso");
    InitShaders();
    InitShadow();
}

void Renderer::InitShadow()
{
    m_shadowMap.Init(m_device, 2048);

    // The shadow root signature is embedded in the depth VS ([RootSignature] attribute).
    auto vs = ReadBlobFile(ResolveShaderPath("ShadowDepth_vs.cso"));
    ASSERT_SUCCEEDED(m_device->CreateRootSignature(0, vs.data(), vs.size(),
                                                   IID_PPV_ARGS(&m_shadowRootSig)));

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.InputLayout = {layout, 1};
    pso.pRootSignature = m_shadowRootSig.Get();
    pso.VS = {vs.data(), vs.size()};   // no PS — depth only
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // casters incl. the double-sided sail
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 3000;                  // constant + slope bias fights shadow acne
    pso.RasterizerState.SlopeScaledDepthBias = 2.5f;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.SampleDesc.Count = 1;
    ASSERT_SUCCEEDED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowPSO)));
}

void Renderer::Shutdown()
{
    m_resourceManager.Shutdown();
    m_linearAllocator.Shutdown();
}

void Renderer::InitShaders()
{
    Material::InitSharedState(m_device, m_rootSignature.Get());
}

void Renderer::BeginFrame(ID3D12CommandAllocator* allocator, ColorBuffer& rt, DepthBuffer& ds,
                          const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor,
                          UINT frameIndex, UINT64 completedFenceValue)
{
    m_frameIndex = frameIndex;
    m_resourceManager.ProcessDeferredDeletions(completedFenceValue);

    m_linearAllocator.SetCurrentFrame(frameIndex);
    m_transientHeap.Reset();

    m_currentRT = &rt;
    m_currentDS = &ds;
    m_mainViewport = viewport;
    m_mainScissor  = scissor;

    if (!m_sceneTargetsInitialized)
    {
        auto w = static_cast<UINT>(viewport.Width);
        auto h = static_cast<UINT>(viewport.Height);
        m_sceneColorCopy.Create(m_device, DXGI_FORMAT_R8G8B8A8_UNORM, w, h,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ASSERT_SUCCEEDED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_sceneColorSRVHeap)));
        m_sceneColorSRVHandle = m_sceneColorSRVHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_sceneColorCopy.GetResource(), &srvDesc, m_sceneColorSRVHandle);

        m_sceneTargetsInitialized = true;
    }

    m_computeContext.Begin(m_computeAllocators[frameIndex].Get());
    m_gfxContext.Begin(allocator);

    auto* cmd = m_gfxContext.GetCommandList();
    m_profiler.BeginFrame((int)frameIndex, cmd);
    m_profiler.BeginScope("Total Frame", cmd);

    m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
    m_gfxContext.SetRootSignature(m_rootSignature.Get());
    m_gfxContext.SetViewportAndScissor(viewport, scissor);

    const float clearColor[] = {0.05f, 0.05f, 0.15f, 1.0f};
    m_gfxContext.ClearRenderTarget(rt, clearColor);
    m_gfxContext.ClearDepth(ds);
    m_gfxContext.SetRenderTarget(rt, ds);
}

void Renderer::RenderScene(Scene& scene, const View& view,
                           const FrameConstants& frameConstants, float elapsedTime)
{
    // Build lights buffer from scene
    std::vector<GpuLight> gpuLights;
    for (const auto& light : scene.GetDirectionalLights())
    {
        gpuLights.push_back(ToGpuLight(light));
    }
    for (const auto& light : scene.GetPointLights())
    {
        gpuLights.push_back(ToGpuLight(light));
    }
    for (const auto& light : scene.GetSpotLights())
    {
        gpuLights.push_back(ToGpuLight(light));
    }

    // Allocate per-frame constants (bind after atmosphere — atmosphere changes the graphics
    // root signature, and restoring it invalidates all graphics root arguments).
    FrameConstants fc = frameConstants;
    fc.numLights = static_cast<int>(gpuLights.size());
    auto frameAlloc = m_linearAllocator.Allocate(sizeof(FrameConstants));
    memcpy(frameAlloc.cpuAddress, &fc, sizeof(FrameConstants));

    // Allocate lights buffer (may be empty).
    LinearAllocator::Allocation lightsAlloc = {};
    if (!gpuLights.empty())
    {
        size_t lightsSize = gpuLights.size() * sizeof(GpuLight);
        lightsAlloc = m_linearAllocator.Allocate(lightsSize);
        memcpy(lightsAlloc.cpuAddress, gpuLights.data(), lightsSize);
    }

    auto* cmd = m_gfxContext.GetCommandList();

    // --- Shadow setup (CPU): fit the cascades to the camera frustum toward the sun. ---
    DirectX::XMFLOAT3 sunTravel = { 0.0f, -1.0f, 0.2f };
    if (!scene.GetDirectionalLights().empty())
        sunTravel = scene.GetDirectionalLights()[0].direction;
    m_shadowMap.ComputeCascades(view, sunTravel);

    // Upload the dynamic rig geometry once; the shadow pass and the main/transparent passes share it.
    const DynamicMesh& rig = scene.GetRig();
    D3D12_VERTEX_BUFFER_VIEW  rigVBV = {};
    D3D12_INDEX_BUFFER_VIEW   rigIBV = {};
    D3D12_GPU_VIRTUAL_ADDRESS rigObjCbv = 0;
    const bool rigValid = rig.visible && !rig.vertices.empty() && !rig.indices.empty();
    bool rigHasTransparent = false;
    if (rigValid)
    {
        size_t vbSize = rig.vertices.size() * sizeof(PbrVertex);
        size_t ibSize = rig.indices.size() * sizeof(uint32_t);
        auto vbAlloc = m_linearAllocator.Allocate(vbSize);
        memcpy(vbAlloc.cpuAddress, rig.vertices.data(), vbSize);
        auto ibAlloc = m_linearAllocator.Allocate(ibSize);
        memcpy(ibAlloc.cpuAddress, rig.indices.data(), ibSize);
        rigVBV = { vbAlloc.gpuAddress, (UINT)vbSize, (UINT)sizeof(PbrVertex) };
        rigIBV = { ibAlloc.gpuAddress, (UINT)ibSize, DXGI_FORMAT_R32_UINT };
        DirectX::XMFLOAT4X4 ident;
        DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
        auto objAlloc = m_linearAllocator.Allocate(sizeof(ident));
        memcpy(objAlloc.cpuAddress, &ident, sizeof(ident));
        rigObjCbv = objAlloc.gpuAddress;
    }

    // Upload the debug wave-sample overlay (world-space markers) the same way; own identity CBV.
    const DynamicMesh& dbg = scene.GetDebug();
    D3D12_VERTEX_BUFFER_VIEW  dbgVBV = {};
    D3D12_INDEX_BUFFER_VIEW   dbgIBV = {};
    D3D12_GPU_VIRTUAL_ADDRESS dbgObjCbv = 0;
    const bool dbgValid = dbg.visible && !dbg.vertices.empty() && !dbg.indices.empty();
    if (dbgValid)
    {
        size_t vbSize = dbg.vertices.size() * sizeof(PbrVertex);
        size_t ibSize = dbg.indices.size() * sizeof(uint32_t);
        auto vbAlloc = m_linearAllocator.Allocate(vbSize);
        memcpy(vbAlloc.cpuAddress, dbg.vertices.data(), vbSize);
        auto ibAlloc = m_linearAllocator.Allocate(ibSize);
        memcpy(ibAlloc.cpuAddress, dbg.indices.data(), ibSize);
        dbgVBV = { vbAlloc.gpuAddress, (UINT)vbSize, (UINT)sizeof(PbrVertex) };
        dbgIBV = { ibAlloc.gpuAddress, (UINT)ibSize, DXGI_FORMAT_R32_UINT };
        DirectX::XMFLOAT4X4 ident;
        DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
        auto objAlloc = m_linearAllocator.Allocate(sizeof(ident));
        memcpy(objAlloc.cpuAddress, &ident, sizeof(ident));
        dbgObjCbv = objAlloc.gpuAddress;
    }

    // Shadow constants (cascade matrices + sampling params), bound to every lit draw.
    struct ShadowCB
    {
        DirectX::XMFLOAT4X4 cascadeVP[ShadowMap::kCascades];
        DirectX::XMFLOAT4   params;   // texelSize, depthBias, cascadeCount, enabled
    };
    ShadowCB scb = {};
    for (int c = 0; c < ShadowMap::kCascades; ++c)
        scb.cascadeVP[c] = m_shadowMap.CascadeViewProjT()[c];
    scb.params = { 1.0f / static_cast<float>(m_shadowMap.GetResolution()), 0.0018f,
                   static_cast<float>(ShadowMap::kCascades), 1.0f };
    auto shadowCbAlloc = m_linearAllocator.Allocate(sizeof(scb));
    memcpy(shadowCbAlloc.cpuAddress, &scb, sizeof(scb));
    D3D12_GPU_VIRTUAL_ADDRESS shadowCbAddr = shadowCbAlloc.gpuAddress;

    // -------------------------------------------------------------------------
    // Async compute phase — atmosphere LUTs + water FFT on the compute queue.
    // The graphics queue issues a GPU-side Wait after this block so it won't
    // consume the outputs until compute has finished signaling.
    // -------------------------------------------------------------------------

    auto& atm   = scene.GetAtmosphere();
    auto& water = scene.GetWaterSurface();

    DirectX::XMFLOAT3 sunDirToward = { 0.0f, 1.0f, 0.0f };
    if (atm.visible)
    {
        DirectX::XMFLOAT3 sunDir = { -0.3f, -1.0f, 0.5f };
        if (!scene.GetDirectionalLights().empty())
            sunDir = scene.GetDirectionalLights()[0].direction;
        // Flip to "toward sun" and normalize (unnormalized causes mu_s > 1 → NaN in Mie phase).
        DirectX::XMVECTOR sdv = DirectX::XMVector3Normalize(
            DirectX::XMVectorSet(-sunDir.x, -sunDir.y, -sunDir.z, 0.0f));
        DirectX::XMStoreFloat3(&sunDirToward, sdv);

        atm.Update(m_computeContext, m_linearAllocator, sunDirToward, view.position.y);
    }

    water.Update(m_computeContext, m_linearAllocator, elapsedTime);

    // Execute compute work and insert a GPU-side dependency into the graphics queue.
    UINT64 computeVal = m_computeContext.Finish(m_computeFence.Get(), m_nextComputeFenceValue);
    m_graphicsQueue->Wait(m_computeFence.Get(), computeVal);

    // Build the spectrum/heightfield debug images (graphics queue) for the ImGui panel.
    water.RenderViz(m_gfxContext, m_linearAllocator);

    // -------------------------------------------------------------------------
    // Shadow depth pass — render casters (hull + rig) into every cascade.
    // -------------------------------------------------------------------------
    RenderShadowPass(scene, view, rigVBV, rigIBV, rigValid);
    // Restore the main render target/viewport/heap/root sig after the shadow pass.
    m_gfxContext.SetRenderTarget(*m_currentRT, *m_currentDS);
    m_gfxContext.SetViewportAndScissor(m_mainViewport, m_mainScissor);
    m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
    m_gfxContext.SetRootSignature(m_rootSignature.Get());

    // -------------------------------------------------------------------------
    // Graphics phase — sky, opaque meshes, water render.
    // -------------------------------------------------------------------------

    if (atm.visible)
    {
        m_profiler.BeginScope("Sky Render", cmd);
        m_gfxContext.SetRenderTarget(*m_currentRT, *m_currentDS);
        atm.Render(m_gfxContext, m_linearAllocator, view);
        m_profiler.EndScope(cmd);

        // Restore renderer state after atmosphere took over descriptor heap and root sig.
        m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
        m_gfxContext.SetRootSignature(m_rootSignature.Get());
    }

    // Bind per-frame root args now that the root signature is stable.
    m_gfxContext.SetConstantBufferView(0, frameAlloc.gpuAddress);
    if (!gpuLights.empty())
        m_gfxContext.SetShaderResourceView(3, lightsAlloc.gpuAddress);

    // Bind cascaded-shadow resources (persist across the lit draws with this root sig).
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu = m_shadowMap.GetArraySRV();
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu = m_transientHeap.CopyDescriptors(m_device, &shadowSrvCpu, 1);
    m_gfxContext.SetConstantBufferView(5, shadowCbAddr);
    m_gfxContext.SetDescriptorTable(6, shadowSrvGpu);

    m_gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_profiler.BeginScope("Opaque Meshes", cmd);
    for (const auto& entity : scene.GetEntities())
    {
        if (!entity.visible)
            continue;
        const auto& mesh = entity.mesh;

        // Per-object world transform at root param 4 (b2). Transposed on upload to match the
        // viewProj convention (HLSL reads column-major; our matrices are DirectXMath row-major).
        {
            DirectX::XMMATRIX worldMat = DirectX::XMLoadFloat4x4(&entity.worldTransform);
            DirectX::XMFLOAT4X4 worldGpu;
            DirectX::XMStoreFloat4x4(&worldGpu, DirectX::XMMatrixTranspose(worldMat));
            auto objAlloc = m_linearAllocator.Allocate(sizeof(worldGpu));
            memcpy(objAlloc.cpuAddress, &worldGpu, sizeof(worldGpu));
            m_gfxContext.SetConstantBufferView(4, objAlloc.gpuAddress);
        }

        m_gfxContext.SetVertexBuffer(mesh->GetVertexBufferView());
        m_gfxContext.SetIndexBuffer(mesh->GetIndexBufferView());

        const auto& materials = mesh->GetMaterials();
        for (const auto& sub : mesh->GetSubMeshes())
        {
            auto& mat = materials[sub.materialIndex];
            mat->Bind(m_gfxContext, m_linearAllocator, m_transientHeap, m_device, m_nullSrvHandle);
            m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, sub.baseVertex, 0);
        }
    }

    // Opaque rig submeshes (spars/lines/foils) using the hoisted rig buffers; the alpha-blended sail
    // is deferred to the transparent pass after the water.
    if (rigValid)
    {
        m_gfxContext.SetVertexBuffer(rigVBV);
        m_gfxContext.SetIndexBuffer(rigIBV);
        m_gfxContext.SetConstantBufferView(4, rigObjCbv);
        for (const auto& sub : rig.submeshes)
        {
            if (sub.material->alphaBlend) { rigHasTransparent = true; continue; }  // deferred
            sub.material->Bind(m_gfxContext, m_linearAllocator, m_transientHeap, m_device, m_nullSrvHandle);
            m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, 0, 0);
        }
    }

    // Debug wave-sample overlay (opaque, world-space verts → identity object matrix). Drawn before
    // the water so the depth test lets the water occlude tiles that sit below its surface.
    if (dbgValid)
    {
        m_gfxContext.SetVertexBuffer(dbgVBV);
        m_gfxContext.SetIndexBuffer(dbgIBV);
        m_gfxContext.SetConstantBufferView(4, dbgObjCbv);
        for (const auto& sub : dbg.submeshes)
        {
            sub.material->Bind(m_gfxContext, m_linearAllocator, m_transientHeap, m_device, m_nullSrvHandle);
            m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, 0, 0);
        }
    }
    m_profiler.EndScope(cmd);

    // Extract light direction from first directional light, fall back to a default.
    DirectX::XMFLOAT3 lightDir = { -0.3f, -1.0f, 0.5f };
    if (!scene.GetDirectionalLights().empty())
        lightDir = scene.GetDirectionalLights()[0].direction;

    if (water.tweaks.visible)
    {
        m_profiler.BeginScope("Scene Copy", cmd);
        m_gfxContext.TransitionResource(*m_currentRT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_gfxContext.TransitionResource(m_sceneColorCopy, D3D12_RESOURCE_STATE_COPY_DEST);
        m_gfxContext.FlushResourceBarriers();
        m_gfxContext.GetCommandList()->CopyResource(m_sceneColorCopy.GetResource(),
                                                    m_currentRT->GetResource());
        m_gfxContext.TransitionResource(*m_currentRT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_gfxContext.TransitionResource(m_sceneColorCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_profiler.EndScope(cmd);

        m_profiler.BeginScope("Water Render", cmd);
        m_gfxContext.TransitionResource(*m_currentDS,
            D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_gfxContext.FlushResourceBarriers();

        water.Render(m_gfxContext, m_linearAllocator, view, lightDir,
                     m_currentDS->GetSRV(), m_sceneColorSRVHandle,
                     m_shadowMap.GetArraySRV(), shadowCbAddr);

        m_gfxContext.TransitionResource(*m_currentDS, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_profiler.EndScope(cmd);

        // Restore renderer heap again after water render.
        m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
        m_gfxContext.SetRootSignature(m_rootSignature.Get());
    }

    // Transparent pass: alpha-blended rig submeshes (the sail) drawn AFTER the water, so they blend
    // over the water/scene rather than only the sky. Reuses the rig VB/IB/CBV from the opaque pass
    // (linear-allocator allocations live the whole frame) and rebinds the per-frame root args.
    if (rigHasTransparent)
    {
        m_profiler.BeginScope("Transparent", cmd);
        m_gfxContext.SetRenderTarget(*m_currentRT, *m_currentDS);
        m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
        m_gfxContext.SetRootSignature(m_rootSignature.Get());
        m_gfxContext.SetConstantBufferView(0, frameAlloc.gpuAddress);
        if (!gpuLights.empty())
            m_gfxContext.SetShaderResourceView(3, lightsAlloc.gpuAddress);
        m_gfxContext.SetConstantBufferView(4, rigObjCbv);
        m_gfxContext.SetConstantBufferView(5, shadowCbAddr);
        m_gfxContext.SetDescriptorTable(6, shadowSrvGpu);
        m_gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_gfxContext.SetVertexBuffer(rigVBV);
        m_gfxContext.SetIndexBuffer(rigIBV);

        for (const auto& sub : scene.GetRig().submeshes)
        {
            if (!sub.material->alphaBlend)
                continue;
            sub.material->Bind(m_gfxContext, m_linearAllocator, m_transientHeap, m_device, m_nullSrvHandle);
            m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, 0, 0);
        }
        m_profiler.EndScope(cmd);
    }

    // Demote shared resources from PSR|NPSR back to NPSR so the next frame's compute
    // queue can re-acquire them without encountering PIXEL_SHADER_RESOURCE states in barriers.
    atm.PrepareForCompute(m_gfxContext);
    water.PrepareForCompute(m_gfxContext);
}

// Depth-only pass: render shadow casters (scene entities + the dynamic rig) into each cascade slice.
void Renderer::RenderShadowPass(Scene& scene, const View& view,
                                const D3D12_VERTEX_BUFFER_VIEW& rigVBV,
                                const D3D12_INDEX_BUFFER_VIEW& rigIBV, bool rigValid)
{
    auto* cmd = m_gfxContext.GetCommandList();
    m_profiler.BeginScope("Shadow", cmd);

    m_gfxContext.FlushResourceBarriers();
    m_shadowMap.Barrier(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    m_gfxContext.SetRootSignature(m_shadowRootSig.Get());
    m_gfxContext.SetPipelineState(m_shadowPSO.Get());
    m_gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT res = m_shadowMap.GetResolution();
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
    m_gfxContext.SetViewportAndScissor(vp, sc);

    const DynamicMesh& rig = scene.GetRig();

    for (int c = 0; c < ShadowMap::kCascades; ++c)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_shadowMap.GetDSV(c);
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        DirectX::XMFLOAT4X4 vpT = m_shadowMap.CascadeViewProjT()[c];
        auto cAlloc = m_linearAllocator.Allocate(sizeof(vpT));
        memcpy(cAlloc.cpuAddress, &vpT, sizeof(vpT));
        m_gfxContext.SetConstantBufferView(0, cAlloc.gpuAddress);   // cascade light view-proj (b0)

        for (const auto& entity : scene.GetEntities())
        {
            if (!entity.visible)
                continue;
            DirectX::XMFLOAT4X4 wT;
            DirectX::XMStoreFloat4x4(&wT, DirectX::XMMatrixTranspose(
                DirectX::XMLoadFloat4x4(&entity.worldTransform)));
            auto oAlloc = m_linearAllocator.Allocate(sizeof(wT));
            memcpy(oAlloc.cpuAddress, &wT, sizeof(wT));
            m_gfxContext.SetConstantBufferView(1, oAlloc.gpuAddress);   // world (b1)
            m_gfxContext.SetVertexBuffer(entity.mesh->GetVertexBufferView());
            m_gfxContext.SetIndexBuffer(entity.mesh->GetIndexBufferView());
            for (const auto& sub : entity.mesh->GetSubMeshes())
                m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, sub.baseVertex, 0);
        }

        if (rigValid)
        {
            DirectX::XMFLOAT4X4 ident;
            DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
            auto oAlloc = m_linearAllocator.Allocate(sizeof(ident));
            memcpy(oAlloc.cpuAddress, &ident, sizeof(ident));
            m_gfxContext.SetConstantBufferView(1, oAlloc.gpuAddress);
            m_gfxContext.SetVertexBuffer(rigVBV);
            m_gfxContext.SetIndexBuffer(rigIBV);
            for (const auto& sub : rig.submeshes)
                m_gfxContext.DrawIndexedInstanced(sub.indexCount, 1, sub.startIndex, 0, 0);
        }
    }

    m_gfxContext.FlushResourceBarriers();
    m_shadowMap.Barrier(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_profiler.EndScope(cmd);
}

UINT64 Renderer::EndFrame(ColorBuffer& rt, ID3D12Fence* fence, UINT64& nextFenceValue)
{
    auto* cmd = m_gfxContext.GetCommandList();
    m_profiler.EndScope(cmd);   // Total Frame
    m_profiler.EndFrame(cmd);   // ResolveQueryData

    m_gfxContext.TransitionResource(rt, D3D12_RESOURCE_STATE_PRESENT);
    return m_gfxContext.Finish(fence, nextFenceValue);
}
