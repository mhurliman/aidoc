#include "Renderer.h"
#include "resources/ColorBuffer.h"
#include "resources/DepthBuffer.h"
#include "resources/Mesh.h"
#include "resources/Material.h"
#include "resources/Texture.h"
#include "resources/Scene.h"
#include "helpers/Assert.h"
#include <cstring>
#include <vector>

void Renderer::Init(ID3D12Device* device, ID3D12CommandQueue* queue, UINT frameCount)
{
    m_device = device;
    m_frameCount = frameCount;

    m_gfxContext.Init(device, queue);

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

    CreateRootSignature();
    InitShaders();
}

void Renderer::Shutdown()
{
    m_resourceManager.Shutdown();
    m_linearAllocator.Shutdown();
}

void Renderer::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER rootParams[4] = {};

    // Root param 0: FrameConstants CBV at b0
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Root param 1: MaterialConstants CBV at b1
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root param 2: Texture descriptor table (t0, t1)
    D3D12_DESCRIPTOR_RANGE srvRanges[2] = {};
    srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[0].NumDescriptors = 1;
    srvRanges[0].BaseShaderRegister = 0;
    srvRanges[0].RegisterSpace = 0;
    srvRanges[0].OffsetInDescriptorsFromTableStart = 0;

    srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[1].NumDescriptors = 1;
    srvRanges[1].BaseShaderRegister = 1;
    srvRanges[1].RegisterSpace = 0;
    srvRanges[1].OffsetInDescriptorsFromTableStart = 1;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 2;
    rootParams[2].DescriptorTable.pDescriptorRanges = srvRanges;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root param 3: Lights StructuredBuffer SRV at t2
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[3].Descriptor.ShaderRegister = 2;
    rootParams[3].Descriptor.RegisterSpace = 0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.ShaderRegister = 0;
    staticSampler.RegisterSpace = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    staticSampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 4;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &staticSampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    ASSERT_SUCCEEDED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &signature, &error));
    ASSERT_SUCCEEDED(m_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                   signature->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_rootSignature)));
}

void Renderer::InitShaders()
{
    Material::InitSharedState(m_device, m_rootSignature.Get());
}

void Renderer::BeginFrame(ID3D12CommandAllocator* allocator, ColorBuffer& rt, DepthBuffer& ds,
                          const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor,
                          UINT frameIndex, UINT64 completedFenceValue)
{
    m_resourceManager.ProcessDeferredDeletions(completedFenceValue);

    m_linearAllocator.SetCurrentFrame(frameIndex);
    m_transientHeap.Reset();

    m_gfxContext.Begin(allocator);
    m_gfxContext.SetDescriptorHeap(m_transientHeap.GetHeap());
    m_gfxContext.SetRootSignature(m_rootSignature.Get());
    m_gfxContext.SetViewportAndScissor(viewport, scissor);

    const float clearColor[] = {0.05f, 0.05f, 0.15f, 1.0f};
    m_gfxContext.ClearRenderTarget(rt, clearColor);
    m_gfxContext.ClearDepth(ds);
    m_gfxContext.SetRenderTarget(rt, ds);
}

void Renderer::RenderScene(const Scene& scene, const View& view,
                           const FrameConstants& frameConstants)
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

    // Bind per-frame constants
    FrameConstants fc = frameConstants;
    fc.numLights = static_cast<int>(gpuLights.size());
    auto frameAlloc = m_linearAllocator.Allocate(sizeof(FrameConstants));
    memcpy(frameAlloc.cpuAddress, &fc, sizeof(FrameConstants));
    m_gfxContext.SetConstantBufferView(0, frameAlloc.gpuAddress);

    // Bind lights structured buffer at root param 3
    if (!gpuLights.empty())
    {
        size_t lightsSize = gpuLights.size() * sizeof(GpuLight);
        auto lightsAlloc = m_linearAllocator.Allocate(lightsSize);
        memcpy(lightsAlloc.cpuAddress, gpuLights.data(), lightsSize);
        m_gfxContext.SetShaderResourceView(3, lightsAlloc.gpuAddress);
    }

    m_gfxContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& entity : scene.GetEntities())
    {
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
}

UINT64 Renderer::EndFrame(ColorBuffer& rt, ID3D12Fence* fence, UINT64& nextFenceValue)
{
    m_gfxContext.TransitionResource(rt, D3D12_RESOURCE_STATE_PRESENT);
    return m_gfxContext.Finish(fence, nextFenceValue);
}
