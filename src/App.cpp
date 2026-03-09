#include "App.h"
#include "Renderer.h"
#include "IRenderer.h"
#include "resources/Mesh.h"
#include "resources/Material.h"
#include "helpers/Assert.h"
#include <cmath>
#include <cstring>
#include <windowsx.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

using namespace DirectX;

App::App() = default;
App::~App() = default;

// Resolve a relative path against the directory containing the executable.
static std::string ResolveExePath(const std::string& relativePath)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        dir = dir.substr(0, pos + 1);
    }
    return dir + relativePath;
}

void App::Init(HWND hwnd)
{
    InitDevice(hwnd);
    InitSwapChain(hwnd);
    InitBuffers();
    InitFence();
    InitViewport();

    m_renderer = std::make_unique<Renderer>();
    m_renderer->Init(m_device.Get(), m_commandQueue.Get(), FrameCount);

    LoadScene();
    InitImGui(hwnd);

    float aspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
    m_camera.SetPerspective(XM_PIDIV4, aspectRatio, 0.1f, 1000.0f);
    m_camera.SetPosition({0.0f, 3.0f, -8.0f});
}

void App::InitDevice(HWND hwnd)
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debugController.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
    }
#endif

    ASSERT_SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ASSERT_SUCCEEDED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
}

void App::InitSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    ASSERT_SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowWidth;
    swapChainDesc.Height = WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    ASSERT_SUCCEEDED(factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &swapChainDesc,
                                                     nullptr, nullptr, &swapChain1));

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ASSERT_SUCCEEDED(swapChain1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::InitBuffers()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ASSERT_SUCCEEDED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; i++)
    {
        m_displayBuffers[i].InitFromSwapChain(m_device.Get(), m_swapChain.Get(), i, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    for (UINT i = 0; i < FrameCount; i++)
    {
        ASSERT_SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    m_depthBuffer.Create(m_device.Get(), WindowWidth, WindowHeight);
}

void App::InitFence()
{
    ASSERT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::InitViewport()
{
    m_viewport = {0.0f, 0.0f, static_cast<float>(WindowWidth), static_cast<float>(WindowHeight),
                  0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight)};
}

void App::LoadScene()
{
    m_scene.Load(ResolveExePath("scenes/testscene.json"), m_device.Get(),
                 m_renderer->GetResourceManager());
}

void App::InitImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = m_renderer->GetTransientHeap().GetHeap();
    initInfo.LegacySingleSrvCpuDescriptor = m_renderer->GetTransientHeap().GetCPUHandle(0);
    initInfo.LegacySingleSrvGpuDescriptor = m_renderer->GetTransientHeap().GetGPUHandle(0);
    ImGui_ImplDX12_Init(&initInfo);
}

void App::BeginInputFrame()
{
    m_inputManager.BeginFrame();
}

void App::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        m_inputManager.OnKeyDown(static_cast<int>(wParam));
        break;
    case WM_KEYUP:
        m_inputManager.OnKeyUp(static_cast<int>(wParam));
        break;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(0);
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(0);
        break;
    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(1);
        break;
    case WM_RBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(1);
        break;
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(2);
        break;
    case WM_MBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(2);
        break;
    case WM_MOUSEMOVE:
        m_inputManager.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        break;
    case WM_MOUSEWHEEL:
        m_inputManager.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    }
}

void App::Update(float dt)
{
    // ImGui gets first priority on input consumption
    if (ImGui::GetCurrentContext())
    {
        if (ImGui::GetIO().WantCaptureKeyboard)
        {
            m_inputManager.ConsumeAllKeys();
        }
        if (ImGui::GetIO().WantCaptureMouse)
        {
            m_inputManager.ConsumeAllMouse();
        }
    }

    m_camera.Update(dt, m_inputManager);

    m_fpsFrameCount++;
    m_fpsAccumulator += dt;
    if (m_fpsAccumulator >= 1.0f)
    {
        m_fps = static_cast<float>(m_fpsFrameCount) / m_fpsAccumulator;
        m_fpsFrameCount = 0;
        m_fpsAccumulator = 0.0f;
    }
}

void App::WaitForGpu()
{
    UINT64 fenceValue = m_nextFenceValue++;
    ASSERT_SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        ASSERT_SUCCEEDED(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    for (UINT i = 0; i < FrameCount; i++)
    {
        m_frameFenceValues[i] = fenceValue;
    }
}

void App::WaitForFrame(UINT frameIndex)
{
    if (m_fence->GetCompletedValue() < m_frameFenceValues[frameIndex])
    {
        ASSERT_SUCCEEDED(
            m_fence->SetEventOnCompletion(m_frameFenceValues[frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void App::Render()
{
    WaitForFrame(m_frameIndex);

    auto& rt = m_displayBuffers[m_frameIndex];

    m_renderer->BeginFrame(m_commandAllocators[m_frameIndex].Get(), rt, m_depthBuffer, m_viewport,
                           m_scissorRect, m_frameIndex, m_fence->GetCompletedValue());

    // Build view
    XMMATRIX viewMat = m_camera.GetViewMatrix();
    XMMATRIX projMat = m_camera.GetProjectionMatrix();

    View view = {};
    XMStoreFloat4x4(&view.viewMatrix, XMMatrixTranspose(viewMat));
    XMStoreFloat4x4(&view.projMatrix, XMMatrixTranspose(projMat));
    XMStoreFloat4x4(&view.viewProjMatrix, XMMatrixTranspose(viewMat * projMat));
    view.position = m_camera.GetPosition();

    // Build per-frame constants
    FrameConstants frameConstants = {};
    frameConstants.viewProj = view.viewProjMatrix;
    frameConstants.cameraPos = view.position;

    m_renderer->RenderScene(m_scene, view, frameConstants);

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

    int entityIdx = 0;
    for (const auto& entity : m_scene.GetEntities())
    {
        ImGui::PushID(entityIdx);
        char entityLabel[128];
        snprintf(entityLabel, sizeof(entityLabel), "Entity %d", entityIdx++);
        if (ImGui::CollapsingHeader(entityLabel, ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto& subMeshes = entity.mesh->GetSubMeshes();
            auto& editMaterials = entity.mesh->GetMaterials();

            if (ImGui::TreeNode("Materials"))
            {
                for (size_t i = 0; i < editMaterials.size(); i++)
                {
                    auto& mat = editMaterials[i];
                    char label[128];
                    if (!mat->name.empty())
                    {
                        snprintf(label, sizeof(label), "[%d] %s", (int)i, mat->name.c_str());
                    }
                    else
                    {
                        snprintf(label, sizeof(label), "Material %d", (int)i);
                    }

                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::TreeNode(label))
                    {
                        ImGui::SliderFloat("Roughness", &mat->roughness, 0.0f, 1.0f);
                        ImGui::SliderFloat("Metallic", &mat->metallic, 0.0f, 1.0f);
                        ImGui::ColorEdit4("Base Color", &mat->baseColorFactor.x);
                        ImGui::ColorEdit3("Emissive", &mat->emissiveFactor.x);
                        ImGui::Text("Base Texture: %s",
                                    mat->HasBaseColorTexture() ? "Yes" : "None");
                        ImGui::Text("Emissive Texture: %s",
                                    mat->HasEmissiveTexture() ? "Yes" : "None");
                        ImGui::Checkbox("Double Sided", &mat->doubleSided);
                        ImGui::Checkbox("Alpha Blend", &mat->alphaBlend);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("SubMeshes"))
            {
                for (size_t i = 0; i < subMeshes.size(); i++)
                {
                    const auto& sub = subMeshes[i];
                    char label[128];
                    if (!sub.name.empty())
                    {
                        snprintf(label, sizeof(label), "[%d] %s", (int)i, sub.name.c_str());
                    }
                    else
                    {
                        snprintf(label, sizeof(label), "SubMesh %d", (int)i);
                    }
                    if (ImGui::TreeNode(label))
                    {
                        ImGui::Text("Material: %d", sub.materialIndex);
                        ImGui::Text("Indices: %u (start: %u)", sub.indexCount, sub.startIndex);
                        ImGui::Text("Base Vertex: %d", sub.baseVertex);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }

    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_renderer->GetCommandList());

    m_frameFenceValues[m_frameIndex] = m_renderer->EndFrame(rt, m_fence.Get(), m_nextFenceValue);

    ASSERT_SUCCEEDED(m_swapChain->Present(m_vsync ? 1 : 0, 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::Cleanup()
{
    WaitForGpu();
    m_renderer->Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}
