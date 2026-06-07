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
    m_device     = device;
    m_frameCount = frameCount;

    m_gfxContext.Init(device, queue);
    m_profiler.Init(device, queue, (int)frameCount);

    m_resourceManager.Init(device, queue);
    m_transientHeap.Init(device, 1024, 1);
    m_linearAllocator.Init(device, 64 * 1024, frameCount);

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

    // Atmosphere: compute LUTs then render sky as background before opaque geometry.
    auto& atm = scene.GetAtmosphere();
    if (atm.visible)
    {
        DirectX::XMFLOAT3 sunDir = { -0.3f, -1.0f, 0.5f };
        if (!scene.GetDirectionalLights().empty())
            sunDir = scene.GetDirectionalLights()[0].direction;
        // Flip to "toward sun" convention and normalize (unnormalized direction causes
        // mu_s > 1 in the Mie phase function, producing NaN via pow(negative, 1.5)).
        DirectX::XMVECTOR sdv = DirectX::XMVector3Normalize(
            DirectX::XMVectorSet(-sunDir.x, -sunDir.y, -sunDir.z, 0.0f));
        DirectX::XMFLOAT3 sunDirToward;
        DirectX::XMStoreFloat3(&sunDirToward, sdv);

        m_profiler.BeginScope("Atmosphere LUT", cmd);
        atm.Update(m_gfxContext, m_linearAllocator, sunDirToward, view.position.y);
        m_profiler.EndScope(cmd);

        m_profiler.BeginScope("Sky Render", cmd);
        // Restore renderer RTV before sky render
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

    m_gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_profiler.BeginScope("Opaque Meshes", cmd);
    for (const auto& entity : scene.GetEntities())
    {
        if (!entity.visible)
            continue;
        const auto& mesh = entity.mesh;
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
    m_profiler.EndScope(cmd);

    // Extract light direction from first directional light, fall back to a default.
    DirectX::XMFLOAT3 lightDir = { -0.3f, -1.0f, 0.5f };
    if (!scene.GetDirectionalLights().empty())
        lightDir = scene.GetDirectionalLights()[0].direction;

    auto& water = scene.GetWaterSurface();
    m_profiler.BeginScope("FFT Update", cmd);
    water.Update(m_gfxContext, m_linearAllocator, elapsedTime);
    m_profiler.EndScope(cmd);

    // Restore renderer state after water compute passes swap the descriptor heap.
    m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
    m_gfxContext.SetRootSignature(m_rootSignature.Get());

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
                     m_currentDS->GetSRV(), m_sceneColorSRVHandle);

        m_gfxContext.TransitionResource(*m_currentDS, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_profiler.EndScope(cmd);

        // Restore renderer heap again after water render.
        m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
        m_gfxContext.SetRootSignature(m_rootSignature.Get());
    }
}

UINT64 Renderer::EndFrame(ColorBuffer& rt, ID3D12Fence* fence, UINT64& nextFenceValue)
{
    auto* cmd = m_gfxContext.GetCommandList();
    m_profiler.EndScope(cmd);   // Total Frame
    m_profiler.EndFrame(cmd);   // ResolveQueryData

    m_gfxContext.TransitionResource(rt, D3D12_RESOURCE_STATE_PRESENT);
    return m_gfxContext.Finish(fence, nextFenceValue);
}
