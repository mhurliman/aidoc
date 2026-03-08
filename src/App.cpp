#include "App.h"
#include "Mesh.h"
#include "Material.h"
#include "Texture.h"
#include <stdexcept>
#include <fstream>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

using namespace DirectX;

// Resolve a relative path against the directory containing the executable.
static std::string ResolveExePath(const std::string& relativePath) {
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
        dir = dir.substr(0, pos + 1);
    return dir + relativePath;
}

static const UINT SceneConstantCount = sizeof(App::SceneConstants) / sizeof(UINT32);
static_assert(sizeof(App::SceneConstants) % 4 == 0, "SceneConstants must be 4-byte aligned");

static inline void ThrowIfFailed(HRESULT hr, const char* msg = "D3D12 call failed") {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) + " (hr=0x" +
            std::to_string(hr) + ")");
    }
}

void App::Init(HWND hwnd) {
    InitDevice(hwnd);
    InitSwapChain(hwnd);
    InitRenderTargets();
    InitDepthBuffer();
    InitCommandList();
    InitFence();
    InitViewport();
    CreatePipelineState();

    m_resourceManager.Init(m_device.Get(), m_commandQueue.Get());
    // Transient heap: 1024 descriptors, slot 0 reserved for ImGui
    m_transientHeap.Init(m_device.Get(), 1024, 1);

    LoadScene();
    InitImGui(hwnd);

    float aspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
    m_camera.SetPerspective(XM_PIDIV4, aspectRatio, 0.1f, 1000.0f);
    m_camera.SetPosition({ 0.0f, 3.0f, -8.0f });
}

void App::InitDevice(HWND hwnd) {
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();

            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debugController.As(&debug1))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
    }
#endif

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc,
        IID_PPV_ARGS(&m_commandQueue)));
}

void App::InitSwapChain(HWND hwnd) {
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowWidth;
    swapChainDesc.Height = WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr,
        &swapChain1));

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ThrowIfFailed(swapChain1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::InitRenderTargets() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc,
        IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; i++) {
        ThrowIfFailed(m_swapChain->GetBuffer(i,
            IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr,
            rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

void App::InitDepthBuffer() {
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc,
        IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = WindowWidth;
    depthDesc.Height = WindowHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&m_depthBuffer)));

    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void App::InitCommandList() {
    for (UINT i = 0; i < FrameCount; i++) {
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    ThrowIfFailed(m_device->CreateCommandList(0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());
}

void App::InitFence() {
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::InitViewport() {
    m_viewport = { 0.0f, 0.0f,
        static_cast<float>(WindowWidth), static_cast<float>(WindowHeight),
        0.0f, 1.0f };
    m_scissorRect = { 0, 0,
        static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight) };
}

void App::CreatePipelineState() {
    // Root param 0: root constants
    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = SceneConstantCount;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Root param 1: descriptor table for base color (t0) + emissive (t1)
    D3D12_DESCRIPTOR_RANGE srvRanges[2] = {};
    srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[0].NumDescriptors = 1;
    srvRanges[0].BaseShaderRegister = 0; // t0
    srvRanges[0].RegisterSpace = 0;
    srvRanges[0].OffsetInDescriptorsFromTableStart = 0;

    srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRanges[1].NumDescriptors = 1;
    srvRanges[1].BaseShaderRegister = 1; // t1
    srvRanges[1].RegisterSpace = 0;
    srvRanges[1].OffsetInDescriptorsFromTableStart = 1;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 2;
    rootParams[1].DescriptorTable.pDescriptorRanges = srvRanges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static sampler (s0)
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
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &staticSampler;
    rootSigDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(0,
        signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));

    auto vsBlob = LoadShaderBlob(ResolveExePath("shaders/pbr_vs.cso"));
    auto psBlob = LoadShaderBlob(ResolveExePath("shaders/pbr_ps.cso"));

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsBlob.data(), vsBlob.size() };
    psoDesc.PS = { psBlob.data(), psBlob.size() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Alpha blending
    auto& rt0 = psoDesc.BlendState.RenderTarget[0];
    rt0.BlendEnable = TRUE;
    rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc,
        IID_PPV_ARGS(&m_pipelineState)));
}

void App::LoadScene() {
    m_mesh = m_resourceManager.Load<Mesh>(ResolveExePath("models/scene.gltf"));
}

void App::InitImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // ImGui uses slot 0 in the transient (shader-visible) heap
    initInfo.SrvDescriptorHeap = m_transientHeap.GetHeap();
    initInfo.LegacySingleSrvCpuDescriptor = m_transientHeap.GetCPUHandle(0);
    initInfo.LegacySingleSrvGpuDescriptor = m_transientHeap.GetGPUHandle(0);
    ImGui_ImplDX12_Init(&initInfo);
}

void App::Update(float dt) {
    m_camera.Update(dt);

    // FPS counter — update once per second
    m_fpsFrameCount++;
    m_fpsAccumulator += dt;
    if (m_fpsAccumulator >= 1.0f) {
        m_fps = static_cast<float>(m_fpsFrameCount) / m_fpsAccumulator;
        m_fpsFrameCount = 0;
        m_fpsAccumulator = 0.0f;
    }
}

void App::WaitForGpu() {
    // Full GPU drain — used only for init/shutdown, not per-frame
    UINT64 fenceValue = m_nextFenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    if (m_fence->GetCompletedValue() < fenceValue) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    // All frames are now complete
    for (UINT i = 0; i < FrameCount; i++)
        m_frameFenceValues[i] = fenceValue;
}

void App::WaitForFrame(UINT frameIndex) {
    // Wait only if the GPU hasn't finished this frame's previous work
    if (m_fence->GetCompletedValue() < m_frameFenceValues[frameIndex]) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_frameFenceValues[frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void App::Render() {
    // Process deferred deletions from previous frames
    m_resourceManager.ProcessDeferredDeletions(m_fence->GetCompletedValue());

    // Wait for this frame's previous submission to complete before reusing its allocator
    WaitForFrame(m_frameIndex);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(),
        m_pipelineState.Get()));

    // Reset transient heap for this frame (preserves reserved slot 0 for ImGui)
    m_transientHeap.Reset();

    // Bind the transient (shader-visible) heap
    ID3D12DescriptorHeap* heaps[] = { m_transientHeap.GetHeap() };
    m_commandList->SetDescriptorHeaps(1, heaps);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.05f, 0.05f, 0.15f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr);

    // Scene rendering
    XMMATRIX view = m_camera.GetViewMatrix();
    XMMATRIX proj = m_camera.GetProjectionMatrix();
    XMFLOAT3 camPos = m_camera.GetPosition();

    float angle = XM_PIDIV4;
    XMFLOAT3 lightDir = { 0.0f, sinf(angle), cosf(angle) };

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto vbv = m_mesh->GetVertexBufferView();
    auto ibv = m_mesh->GetIndexBufferView();
    m_commandList->IASetVertexBuffers(0, 1, &vbv);
    m_commandList->IASetIndexBuffer(&ibv);

    // Null SRV for materials without textures — allocate once in persistent heap
    // and reuse. We create it here but it persists across frames since the
    // persistent heap slot is stable.
    static D3D12_CPU_DESCRIPTOR_HANDLE nullSrvHandle = {};
    static bool nullSrvCreated = false;
    static UINT nullSrvIndex = UINT_MAX;
    if (!nullSrvCreated) {
        nullSrvHandle = m_resourceManager.GetSRVHeap().Allocate(nullSrvIndex);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
        nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &nullDesc, nullSrvHandle);
        nullSrvCreated = true;
    }

    const auto& materials = m_mesh->GetMaterials();
    for (const auto& sub : m_mesh->GetSubMeshes()) {
        const auto& mat = materials[sub.materialIndex];

        SceneConstants constants = {};
        XMStoreFloat4x4(&constants.viewProj, XMMatrixTranspose(view * proj));
        constants.cameraPos = camPos;
        constants.roughness = mat->roughness;
        constants.lightDir = lightDir;
        constants.metallic = mat->metallic;
        constants.baseColorFactor = mat->baseColorFactor;
        constants.hasTexture = mat->HasBaseColorTexture() ? 1 : 0;
        constants.emissiveFactor = mat->emissiveFactor;
        constants.hasEmissiveTexture = mat->HasEmissiveTexture() ? 1 : 0;

        m_commandList->SetGraphicsRoot32BitConstants(0, SceneConstantCount,
            &constants, 0);

        // Copy material's texture descriptors from persistent → transient heap
        D3D12_CPU_DESCRIPTOR_HANDLE srcHandles[2];

        srcHandles[0] = mat->baseColorTexture
            ? mat->baseColorTexture->GetSRVHandle() : nullSrvHandle;
        srcHandles[1] = mat->emissiveTexture
            ? mat->emissiveTexture->GetSRVHandle() : nullSrvHandle;

        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
            m_transientHeap.CopyDescriptors(m_device.Get(), srcHandles, 2);
        m_commandList->SetGraphicsRootDescriptorTable(1, gpuHandle);

        m_commandList->DrawIndexedInstanced(sub.indexCount, 1,
            sub.startIndex, sub.baseVertex, 0);
    }

    // ImGui
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Scene Inspector");
    ImGui::Text("%.1f FPS (%.2f ms)", m_fps, m_fps > 0.0f ? 1000.0f / m_fps : 0.0f);
    XMFLOAT3 pos = m_camera.GetPosition();
    ImGui::Text("Camera: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
    ImGui::SliderFloat("Move Speed", &m_camera.GetMoveSpeed(), 1.0f, 50.0f);
    ImGui::Checkbox("VSync", &m_vsync);
    ImGui::Separator();

    const auto& subMeshes = m_mesh->GetSubMeshes();
    auto& editMaterials = m_mesh->GetMaterials();

    if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < editMaterials.size(); i++) {
            auto& mat = editMaterials[i];
            char label[128];
            if (!mat->name.empty())
                snprintf(label, sizeof(label), "[%d] %s", (int)i, mat->name.c_str());
            else
                snprintf(label, sizeof(label), "Material %d", (int)i);
            if (ImGui::TreeNode(label)) {
                ImGui::SliderFloat("Roughness", &mat->roughness, 0.0f, 1.0f);
                ImGui::SliderFloat("Metallic", &mat->metallic, 0.0f, 1.0f);
                ImGui::ColorEdit4("Base Color", &mat->baseColorFactor.x);
                ImGui::ColorEdit3("Emissive", &mat->emissiveFactor.x);
                ImGui::Text("Base Texture: %s", mat->HasBaseColorTexture() ? "Yes" : "None");
                ImGui::Text("Emissive Texture: %s", mat->HasEmissiveTexture() ? "Yes" : "None");
                ImGui::Checkbox("Double Sided", &mat->doubleSided);
                ImGui::Checkbox("Alpha Blend", &mat->alphaBlend);
                ImGui::TreePop();
            }
        }
    }

    if (ImGui::CollapsingHeader("SubMeshes")) {
        for (size_t i = 0; i < subMeshes.size(); i++) {
            const auto& sub = subMeshes[i];
            char label[128];
            if (!sub.name.empty())
                snprintf(label, sizeof(label), "[%d] %s", (int)i, sub.name.c_str());
            else
                snprintf(label, sizeof(label), "SubMesh %d", (int)i);
            if (ImGui::TreeNode(label)) {
                ImGui::Text("Material: %d", sub.materialIndex);
                ImGui::Text("Indices: %u (start: %u)", sub.indexCount, sub.startIndex);
                ImGui::Text("Base Vertex: %d", sub.baseVertex);
                ImGui::TreePop();
            }
        }
    }

    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    ThrowIfFailed(m_swapChain->Present(m_vsync ? 1 : 0, 0));

    // Signal the fence for this frame with a globally monotonic value — no CPU wait here
    m_frameFenceValues[m_frameIndex] = m_nextFenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_frameFenceValues[m_frameIndex]));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

std::vector<BYTE> App::LoadShaderBlob(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + filename);
    }
    auto size = file.tellg();
    std::vector<BYTE> blob(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(blob.data()), size);
    return blob;
}

void App::Cleanup() {
    WaitForGpu();
    m_mesh.reset();
    m_resourceManager.Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}
