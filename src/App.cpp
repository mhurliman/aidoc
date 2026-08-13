#include "App.h"
#include "Renderer.h"
#include "IRenderer.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "assets/ObjLoader.h"
#include "util/Assert.h"
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
    LoadProps();
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

void App::Resize(UINT width, UINT height)
{
    // Ignore pre-init messages, minimize (0×0), and no-op resizes.
    if (!m_swapChain || width == 0 || height == 0)
        return;
    if (width == m_width && height == m_height)
        return;

    // All in-flight GPU work must finish before back buffers / depth are released.
    WaitForGpu();

    // Drop references to the old swap-chain back buffers (required by ResizeBuffers).
    for (UINT i = 0; i < FrameCount; ++i)
        m_displayBuffers[i].Reset();

    ASSERT_SUCCEEDED(m_swapChain->ResizeBuffers(
        FrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));

    // Recreate RTVs into the existing heap and the depth buffer at the new size.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i)
    {
        m_displayBuffers[i].InitFromSwapChain(m_device.Get(), m_swapChain.Get(), i, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    m_depthBuffer.Create(m_device.Get(), width, height);

    m_width  = width;
    m_height = height;
    m_viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

    m_camera.SetPerspective(XM_PIDIV4,
        static_cast<float>(width) / static_cast<float>(height), 0.1f, 1000.0f);

    // Size-dependent render targets (scene-color copy) are rebuilt on the next BeginFrame.
    m_renderer->OnResize();
}

void App::LoadScene()
{
    m_scene.Load(ResolveExePath("scenes/testscene.json"), m_device.Get(),
                 m_renderer->GetResourceManager());

    WaterDesc waterDesc = {};
    waterDesc.N = waterDesc.M = 256;
    waterDesc.TileSize = 512.0f;  // mesh tile span = largest cascade; cascades sized internally
    m_scene.CreateWater(m_device.Get(), waterDesc, m_commandQueue.Get());

    m_scene.CreateAtmosphere(m_device.Get());

    // Prime the directional light direction from the initial GUI sun angles so that
    // frame 0 uses the same direction the sliders show rather than whatever came from JSON.
    if (!m_scene.GetDirectionalLights().empty())
    {
        float elevRad = m_sunElevation * 3.14159265f / 180.0f;
        float azimRad = m_sunAzimuth   * 3.14159265f / 180.0f;
        auto& dir = m_scene.GetDirectionalLights()[0].direction;
        dir.x = -cosf(elevRad) * sinf(azimRad);
        dir.y = -sinf(elevRad);
        dir.z = -cosf(elevRad) * cosf(azimRad);
    }

    // Wire the atmosphere env cubemap into the water reflection slot.
    // This replaces the static JPG cubemap; the atmosphere updates it every frame.
    m_scene.GetWaterSurface().SetEnvCubemapFromResource(
        m_device.Get(), m_scene.GetAtmosphere().GetEnvCubeResource());
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
    case WM_SIZE:
        // wParam == SIZE_MINIMIZED yields 0×0 via LOWORD/HIWORD, which Resize() ignores.
        Resize(LOWORD(lParam), HIWORD(lParam));
        break;
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

// Load a floating OBJ prop and drop it into the scene in front of the camera.
void App::LoadProps()
{
    struct ModelDef { const char* file; XMFLOAT4 color; };
    const ModelDef models[] = {
        { "models/suzanne.obj",   { 0.80f, 0.45f, 0.20f, 1.0f } },
        { "models/spot.obj",      { 0.85f, 0.75f, 0.55f, 1.0f } },
        { "models/teapot.obj",    { 0.55f, 0.65f, 0.85f, 1.0f } },
        { "models/armadillo.obj", { 0.55f, 0.72f, 0.50f, 1.0f } },
        { "models/happy.obj",     { 0.80f, 0.72f, 0.86f, 1.0f } },
    };

    srand(0x5EA0CEA1u);            // deterministic scatter layout
    const float kArea   = 22.0f;   // half-extent (m) of the scatter square around the origin
    const float kTarget = 3.0f;    // approximate world size each model is normalized to
    auto rnd01 = []() { return (float)rand() / (float)RAND_MAX; };

    for (const ModelDef& md : models)
    {
        std::vector<float> pos, nrm;
        std::vector<uint32_t> idx;
        if (!LoadObj(ResolveExePath(md.file), pos, nrm, idx))
        {
            fprintf(stderr, "[prop] failed to load %s; skipping.\n", md.file);
            continue;
        }

        // AABB → pivot-independent center + a scale that normalizes every model to ~kTarget metres.
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i + 2 < pos.size(); i += 3)
            for (int k = 0; k < 3; ++k)
            {
                mn[k] = pos[i + k] < mn[k] ? pos[i + k] : mn[k];
                mx[k] = pos[i + k] > mx[k] ? pos[i + k] : mx[k];
            }
        float ext = 1e-4f;
        for (int k = 0; k < 3; ++k) ext = (mx[k] - mn[k]) > ext ? (mx[k] - mn[k]) : ext;

        auto mat = m_renderer->GetResourceManager().CreateMaterial(md.file);
        mat->baseColorFactor = md.color;
        mat->roughness    = 0.5f;
        mat->metallic     = 0.0f;
        mat->doubleSided  = true;
        mat->shadingModel = ShadingModel::PBR;
        mat->CreatePSO(m_device.Get());

        auto mesh = std::make_shared<Mesh>();
        mesh->CreateFromArrays(pos.data(), nrm.data(), static_cast<uint32_t>(pos.size() / 3),
                               idx.data(), static_cast<uint32_t>(idx.size()), mat, m_device.Get());

        Entity e;
        e.mesh    = mesh;
        e.visible = true;
        XMStoreFloat4x4(&e.worldTransform, XMMatrixIdentity());
        m_scene.GetEntities().push_back(e);

        FloatingProp fp;
        fp.entity = static_cast<int>(m_scene.GetEntities().size()) - 1;
        fp.x      = (rnd01() * 2.0f - 1.0f) * kArea;
        fp.z      = (rnd01() * 2.0f - 1.0f) * kArea;
        fp.yaw    = rnd01() * 6.2831853f;
        fp.scale  = kTarget / ext;
        for (int k = 0; k < 3; ++k) fp.center[k] = 0.5f * (mn[k] + mx[k]);
        m_props.push_back(fp);
    }

    SetupWaveDebug();
}

// ---- Wave debug overlay ------------------------------------------------------------------------
// Tunables for the CPU-vs-GPU surface overlay (a grid of small tiles around the prop).
namespace { constexpr int kDebugGrid = 20; constexpr int kDebugModes = 32; constexpr float kDebugSide = 16.0f; }

// Build the overlay's fixed topology (two overlaid tile grids) + its materials once.
void App::SetupWaveDebug()
{
    auto makeDbgMat = [&](const char* key, XMFLOAT4 col) {
        auto m = m_renderer->GetResourceManager().CreateMaterial(key);
        m->baseColorFactor = col;
        m->emissiveFactor  = { col.x * 0.6f, col.y * 0.6f, col.z * 0.6f };  // glow so it reads over water
        m->roughness    = 0.6f;
        m->metallic     = 0.0f;
        m->doubleSided  = true;
        m->shadingModel = ShadingModel::PBR;
        m->CreatePSO(m_device.Get());
        return m;
    };
    m_physSurfMat  = makeDbgMat("wave_phys", { 0.15f, 0.95f, 0.25f, 1.0f });  // green  = buoyancy
    m_waveDebugMat = makeDbgMat("wave_gpu",  { 1.00f, 0.10f, 0.90f, 1.0f });  // magenta = GPU surface

    const int tiles = kDebugGrid * kDebugGrid;
    m_dbgH.resize(tiles); m_dbgDx.resize(tiles); m_dbgDz.resize(tiles);

    auto& dbg = m_scene.GetDebug();
    dbg.vertices.resize(static_cast<size_t>(tiles) * 4 * 2);   // [green tiles][magenta tiles]
    dbg.indices.clear();
    dbg.indices.reserve(static_cast<size_t>(tiles) * 6 * 2);
    for (int t = 0; t < tiles * 2; ++t)
    {
        uint32_t b = static_cast<uint32_t>(t) * 4;
        dbg.indices.push_back(b + 0); dbg.indices.push_back(b + 1); dbg.indices.push_back(b + 2);
        dbg.indices.push_back(b + 0); dbg.indices.push_back(b + 2); dbg.indices.push_back(b + 3);
    }
    uint32_t perSurf = static_cast<uint32_t>(tiles) * 6;
    dbg.submeshes = {
        { 0u,      perSurf, m_physSurfMat },   // green:   verts [0, tiles*4)
        { perSurf, perSurf, m_waveDebugMat },  // magenta: verts [tiles*4, 2*tiles*4)
    };
    dbg.visible = false;
}

// Refill the overlay each frame: green = exactly what buoyancy samples (SampleHeightCPU, modes=6,
// un-displaced); magenta = the GPU/render surface (high-mode height + choppiness displacement,
// placed at the displaced position like the water vertex shader).
void App::UpdateDebugWaveMesh()
{
    auto& dbg = m_scene.GetDebug();
    dbg.visible = m_showWaveDebug;
    if (!m_showWaveDebug || dbg.vertices.empty()) return;

    auto& water = m_scene.GetWaterSurface();
    constexpr int DG = kDebugGrid;
    const int   tiles = DG * DG;
    const float cell  = kDebugSide / (DG - 1);
    const float ox    = m_propX - kDebugSide * 0.5f;
    const float oz    = m_propZ - kDebugSide * 0.5f;

    water.SampleSurfaceGrid(ox, oz, cell, DG, m_elapsedTime, kDebugModes,
                            m_dbgH.data(), m_dbgDx.data(), m_dbgDz.data());

    constexpr float half = 0.12f;
    auto writeTile = [&](int base, float wx, float h, float wz)
    {
        auto set = [&](int k, float dx, float dz)
        {
            PbrVertex& v = dbg.vertices[base + k];
            v.position[0] = wx + dx; v.position[1] = h; v.position[2] = wz + dz;
            v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
            v.uv[0] = v.uv[1] = 0.0f;
        };
        set(0, -half, -half); set(1, half, -half); set(2, half, half); set(3, -half, half);
    };

    for (int j = 0; j < DG; ++j)
        for (int i = 0; i < DG; ++i)
        {
            int   idx = j * DG + i;
            float gx  = ox + i * cell, gz = oz + j * cell;
            // GREEN — exactly the buoyancy sample used to float the prop (modes match UpdateProps).
            float hp = water.SampleHeightCPU(gx, gz, m_elapsedTime, 6);
            writeTile(idx * 4, gx, hp, gz);
            // MAGENTA — GPU/render surface: displaced like the water vertex shader.
            writeTile((tiles + idx) * 4, gx + m_dbgDx[idx], m_dbgH[idx], gz + m_dbgDz[idx]);
        }
}

// Single-point buoyancy: sit each prop on the water at its fixed XZ and tilt it to the local
// surface normal (finite-differenced from the CPU wave height). Cheap and reads convincingly.
void App::UpdateProps()
{
    auto& water = m_scene.GetWaterSurface();
    const int   modes = 6;      // low-frequency swell that dominates buoyancy
    const float e     = 0.75f;  // finite-difference step (m) for the surface slope

    for (const FloatingProp& p : m_props)
    {
        float h   = water.SampleHeightCPU(p.x,     p.z,     m_elapsedTime, modes);
        float hxp = water.SampleHeightCPU(p.x + e, p.z,     m_elapsedTime, modes);
        float hxm = water.SampleHeightCPU(p.x - e, p.z,     m_elapsedTime, modes);
        float hzp = water.SampleHeightCPU(p.x,     p.z + e, m_elapsedTime, modes);
        float hzm = water.SampleHeightCPU(p.x,     p.z - e, m_elapsedTime, modes);

        XMVECTOR up = XMVector3Normalize(
            XMVectorSet(-(hxp - hxm) / (2.0f * e), 1.0f, -(hzp - hzm) / (2.0f * e), 0.0f));
        XMVECTOR fwdRef = XMVectorSet(sinf(p.yaw), 0.0f, cosf(p.yaw), 0.0f);  // random heading
        XMVECTOR right  = XMVector3Normalize(XMVector3Cross(up, fwdRef));
        XMVECTOR fwd    = XMVector3Cross(right, up);

        XMMATRIX rot;
        rot.r[0] = XMVectorSetW(right, 0.0f);
        rot.r[1] = XMVectorSetW(up,    0.0f);
        rot.r[2] = XMVectorSetW(fwd,   0.0f);
        rot.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

        // Center the geometry (T(-center)) before scale/rotate so any pivot floats at (x, h, z),
        // then sink by m_propSink. This puts the model ON the surface rather than above it.
        XMMATRIX world = XMMatrixTranslation(-p.center[0], -p.center[1], -p.center[2]) *
                         XMMatrixScaling(p.scale, p.scale, p.scale) * rot *
                         XMMatrixTranslation(p.x, h + m_propSink, p.z);
        XMStoreFloat4x4(&m_scene.GetEntities()[p.entity].worldTransform, world);
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

    if (m_inputManager.IsKeyPressed('P'))
        m_timePaused = !m_timePaused;

    m_camera.Update(dt, m_inputManager);
    if (!m_timePaused)
        m_elapsedTime += dt;   // freezing this holds the whole sea (render + buoyancy) still
    UpdateProps();
    UpdateDebugWaveMesh();

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

    // Mesh rebuild releases the old vertex/index buffers — must wait for all frame slots,
    // not just this one, since FrameCount=3 means two other slots may still be in flight.
    if (m_scene.GetWaterSurface().tweaks.meshResolution != m_scene.GetWaterSurface().GetDesc().MeshResolution)
        WaitForGpu();
    m_scene.GetWaterSurface().RebuildMeshIfNeeded();

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
    view.nearZ    = 0.1f;
    view.farZ     = 1000.0f;

    // Build per-frame constants
    FrameConstants frameConstants = {};
    frameConstants.viewProj = view.viewProjMatrix;
    frameConstants.cameraPos = view.position;

    m_renderer->RenderScene(m_scene, view, frameConstants, m_elapsedTime);

    // ImGui
    m_renderer->GetProfiler().BeginScope("ImGui", m_renderer->GetCommandList());

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Scene Inspector");
    ImGui::Text("%.1f FPS (%.2f ms)", m_fps, m_fps > 0.0f ? 1000.0f / m_fps : 0.0f);
    XMFLOAT3 pos = m_camera.GetPosition();
    ImGui::Text("Camera: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
    ImGui::SliderFloat("Move Speed", &m_camera.GetMoveSpeed(), 1.0f, 50.0f);
    ImGui::Checkbox("VSync", &m_vsync);
    ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume (P)" : "Pause (P)"))
        m_timePaused = !m_timePaused;
    ImGui::Separator();

    int entityIdx = 0;
    for (auto& entity : m_scene.GetEntities())
    {
        ImGui::PushID(entityIdx);
        char entityLabel[128];
        snprintf(entityLabel, sizeof(entityLabel), "Entity %d", entityIdx++);
        if (ImGui::CollapsingHeader(entityLabel, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Visible", &entity.visible);
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

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& water  = m_scene.GetWaterSurface();
        auto& tweaks = water.tweaks;
        const auto& desc = water.GetDesc();

        ImGui::Checkbox("Visible##water", &tweaks.visible);
        ImGui::SameLine();
        ImGui::Checkbox("Tessellation", &tweaks.tessellation);
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &tweaks.wireframe);
        ImGui::SliderInt("Tile Count",      &tweaks.tileCount,      1,  9);
        ImGui::SliderInt("Mesh Resolution", &tweaks.meshResolution, 4, 512);
        ImGui::SliderFloat("Wind Speed",        &tweaks.windSpeed,       0.1f, 100.0f);
        ImGui::SliderAngle("Wind Direction",    &tweaks.windTheta,       -180.0f, 180.0f);
        ImGui::SliderFloat("Directionality",    &tweaks.directionality,  1.0f, 8.0f, "%.1f");
        ImGui::SliderFloat("Amplitude",         &tweaks.amplitude,       0.0f, 0.25f, "%.3f");
        ImGui::SliderFloat("Small Wave Cutoff", &tweaks.smallWaveCutoff, 0.001f, 0.1f, "%.4f");
        ImGui::SliderFloat("Choppiness",        &tweaks.choppiness,      0.0f,   1.0f);
        ImGui::SliderFloat("Time Scale",        &tweaks.timeScale,       0.0f,   2.0f);
        ImGui::SliderFloat("Max Tess",          &tweaks.maxTessellation, 1.0f,   64.0f);
        ImGui::SliderFloat("Tess Distance",     &tweaks.tessDistance,    0.0f, 1000.0f);
        ImGui::ColorEdit3 ("Color",             &tweaks.color.x);
        ImGui::SliderFloat("Sky Ambient",       &tweaks.ambient,         0.0f, 2.0f, "%.2f");

        // Per-cascade debug images: time-evolved spectrum h(k,t) (left) + heightfield (right).
        // GPU textures shown via a per-frame SRV copied into the ImGui-bound transient heap.
        ImGui::Spacing();
        ImGui::TextDisabled("Spectrum h(k,t)      Heightfield   (per cascade)");
        {
            auto& th = m_renderer->GetTransientHeap();
            const ImVec2 imgSize(104.0f, 104.0f);
            for (int c = 0; c < 3; ++c)
            {
                // Images first (so the two columns align under the header), toggle in the right column.
                D3D12_CPU_DESCRIPTOR_HANDLE specCpu = water.GetSpectrumVizSRV(c);
                D3D12_CPU_DESCRIPTOR_HANDLE hgtCpu  = water.GetHeightVizSRV(c);
                D3D12_GPU_DESCRIPTOR_HANDLE specGpu = th.CopyDescriptors(m_device.Get(), &specCpu, 1);
                D3D12_GPU_DESCRIPTOR_HANDLE hgtGpu  = th.CopyDescriptors(m_device.Get(), &hgtCpu, 1);
                ImGui::Image((ImTextureID)specGpu.ptr, imgSize);
                ImGui::SameLine();
                ImGui::Image((ImTextureID)hgtGpu.ptr, imgSize);
                ImGui::SameLine();

                // Right column: apply toggle + wavelength band label (shortest = 2·tile/N).
                float tile = water.GetCascadeTileSize(c);
                float shortLambda = 2.0f * tile / water.GetVizSize();
                char lbl[48];
                snprintf(lbl, sizeof(lbl), "C%d  %.2g-%.0f m##casc", c, shortLambda, tile);
                ImGui::Checkbox(lbl, &tweaks.cascadeEnabled[c]);   // apply this cascade to the surface
            }
            ImGui::SliderFloat("Spectrum gain", &tweaks.spectrumVizGain, 0.1f, 500.0f, "%.1f",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Height gain",   &tweaks.heightVizGain,   0.1f, 50.0f, "%.2f",
                               ImGuiSliderFlags_Logarithmic);
        }

        ImGui::TextDisabled("FFT: %dx%d  Tile: %.1fm", desc.N, desc.M, desc.TileSize);

        ImGui::SeparatorText("Buoyancy debug");
        ImGui::Checkbox("Wave debug: green=buoyancy (CPU), magenta=GPU surface", &m_showWaveDebug);
        ImGui::SliderFloat("Prop sink", &m_propSink, -2.0f, 2.0f, "%.2f");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& atm = m_scene.GetAtmosphere();
        auto& ap  = atm.params;
        ImGui::Checkbox("Visible##atm", &atm.visible);

        if (ImGui::TreeNode("Sun"))
        {
            // Drive the first directional light's direction from elevation/azimuth.
            bool changed = false;
            changed |= ImGui::SliderFloat("Elevation (deg)", &m_sunElevation, -10.0f, 90.0f);
            changed |= ImGui::SliderFloat("Azimuth (deg)",   &m_sunAzimuth,    0.0f, 360.0f);
            ImGui::SliderFloat("Intensity", &ap.sunIntensity, 1.0f, 50.0f);

            if (changed && !m_scene.GetDirectionalLights().empty())
            {
                float elevRad = m_sunElevation * 3.14159265f / 180.0f;
                float azimRad = m_sunAzimuth   * 3.14159265f / 180.0f;
                // Direction light travels (toward scene): opposite of "toward sun"
                auto& dir = m_scene.GetDirectionalLights()[0].direction;
                dir.x = -cosf(elevRad) * sinf(azimRad);
                dir.y = -sinf(elevRad);
                dir.z = -cosf(elevRad) * cosf(azimRad);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Rayleigh"))
        {
            ImGui::SliderFloat3("Beta (km^-1)", &ap.rayleighBeta.x, 0.0f, 0.1f, "%.5f");
            ImGui::SliderFloat("Scale Height (km)", &ap.rayleighScaleHeight, 1.0f, 20.0f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Mie"))
        {
            ImGui::SliderFloat("Beta (km^-1)",    &ap.mieBeta.x,      0.0f, 0.1f, "%.4f");
            ap.mieBeta.y = ap.mieBeta.z = ap.mieBeta.x;
            ap.mieBetaExt.x = ap.mieBetaExt.y = ap.mieBetaExt.z = ap.mieBeta.x * 1.1f;
            ImGui::SliderFloat("Scale Height (km)", &ap.mieScaleHeight, 0.1f, 5.0f);
            ImGui::SliderFloat("G (asymmetry)",  &ap.mieG,           0.0f, 0.999f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Ozone"))
        {
            ImGui::SliderFloat3("Beta Abs (km^-1)",   &ap.ozoneBetaAbs.x,      0.0f, 0.005f, "%.5f");
            ImGui::SliderFloat("Center Height (km)", &ap.ozoneCenterHeight, 10.0f, 40.0f);
            ImGui::SliderFloat("Width (km)",          &ap.ozoneWidth,         5.0f, 30.0f);
            ImGui::TreePop();
        }
        if (ImGui::Button("Recompute Transmittance LUT"))
            atm.MarkDirty();
    }

    ImGui::End();

    // GPU Timings window
    ImGui::Begin("GPU Timings");
    const auto& gpuResults = m_renderer->GetProfiler().GetResults();
    if (gpuResults.empty())
    {
        ImGui::TextDisabled("Waiting for GPU data...");
    }
    else
    {
        float totalMs = 1.0f;
        for (const auto& r : gpuResults)
            if (strcmp(r.name, "Total Frame") == 0) { totalMs = r.ms; break; }

        if (ImGui::BeginTable("gpu_timings", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("ms",    ImGuiTableColumnFlags_WidthFixed,  52.0f);
            ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& r : gpuResults)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", r.ms);
                ImGui::TableSetColumnIndex(2);
                ImGui::ProgressBar(r.ms / totalMs, ImVec2(-1.0f, 0.0f), "");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_renderer->GetCommandList());
    m_renderer->GetProfiler().EndScope(m_renderer->GetCommandList()); // ImGui

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
