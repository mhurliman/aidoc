#include "App.h"
#include "Renderer.h"
#include "IRenderer.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "assets/ObjLoader.h"
#include "util/Assert.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
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
    InitSim();
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

            // GPU-based validation is a bug-hunting tool, not a default. Measured here it costs
            // ~11 ms a frame (Debug went 90 fps -> 44 fps with it on), and it inflates the GPU
            // timings too, which makes it look like a rendering problem rather than an instrument.
            // The ordinary debug layer above stays on. Turn this back on with
            // -DGPU_BASED_VALIDATION=ON when chasing a GPU-side fault.
#if defined(ENABLE_GPU_BASED_VALIDATION)
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debugController.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
#endif
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
namespace {

// ---- Laser rig geometry, in the SIM's body frame (+x bow, +y port, +z up) ----------------------
// Copied from Sim.Models/Composition/LaserBoat.cs and LaserDefaults.cs. The ABI publishes the mast
// curve and the sail, but not where the rig is bolted to the hull, so these are duplicated here and
// WILL drift if the sim's rig moves. They are the numbers to check first if the boom or the vang
// starts floating off the boat.
const XMFLOAT3 kMastBase_S      = { 0.80f,  0.0f,  0.0f   };
constexpr float kMastHeight_m   = 5.810f;
const XMFLOAT3 kGooseneck_S     = { 0.80f,  0.0f,  0.595f };
constexpr float kBoomLength_m   = 2.74f;
constexpr float kBailInboard_m  = 0.0f;    // bail sits at the clew on a Laser
constexpr float kVangOnBoom_m   = 0.475f;  // along the boom from the gooseneck
const XMFLOAT3 kVangHull_S      = { 0.80f,  0.0f,  0.095f };
const XMFLOAT3 kHullBlock_S     = {-1.60f,  0.0f,  0.40f  };

// The forward lead. A Laser's mainsheet does not stop at the transom block: it runs forward under
// the boom to a block at the gooseneck and down to the deck ratchet, which is the part the sailor
// actually holds. The PHYSICS models none of this - the sim spans bail to hull block and folds the
// rest into SheetPurchase (2.5) - so these two runs are cosmetic, and moving them cannot change how
// the boat sails. Positions are estimated from the rig dimensions rather than read from the sim,
// which has no deck-block geometry to read.
const XMFLOAT3 kSheetGooseBlock_S = { 0.80f, 0.0f, 0.545f };  // just under the gooseneck
const XMFLOAT3 kSheetDeckBlock_S  = { 0.50f, 0.0f, 0.105f };  // deck ratchet, aft of the mast
const XMFLOAT3 kBridlePort_S    = {-1.95f,  0.30f, 0.10f  };
const XMFLOAT3 kBridleStbd_S    = {-1.95f, -0.30f, 0.10f  };

// Foils. Also from the sim's own visual defaults; the blades are not on the wire either.
const XMFLOAT3 kKeelRoot_S      = { 0.15f,  0.0f,  0.0f   };
constexpr float kKeelRootChord = 0.25f, kKeelTipChord = 0.175f;
constexpr float kKeelSpan = 1.0f, kKeelThick = 0.02f, kKeelRake = 0.314f;
const XMFLOAT3 kRudderStock_S   = {-2.16f,  0.0f,  0.10f  };
constexpr float kRudderRootChord = 0.15f, kRudderTipChord = 0.105f;
constexpr float kRudderSpan = 0.65f, kRudderThick = 0.015f, kRudderRake = 0.471f;

constexpr float kCrewMarkerRadius = 0.12f;
constexpr float kTelltaleLength = 0.26f, kTelltaleRadius = 0.007f;

// The masthead fly's outline in its (wind-from, up) plane: tail rectangle then arrowhead.
const float kFlyOutline[7][2] = {
    {-0.25f,  0.035f}, {-0.25f, -0.035f}, { 0.06f, -0.035f}, { 0.06f, 0.035f},
    { 0.06f, -0.11f },  { 0.34f,  0.0f  }, { 0.06f,  0.11f },
};

// Sim body frame (+z up) -> renderer body frame (+y up). The rig maths below is done in the sim's
// frame so it matches the physics kinematics line for line; every result crosses here exactly once.
XMVECTOR SimToRender(XMVECTOR v)
{
    return XMVectorSet(XMVectorGetX(v), XMVectorGetZ(v), XMVectorGetY(v), 0.0f);
}
XMVECTOR SimConst(const XMFLOAT3& p) { return XMVectorSet(p.x, p.y, p.z, 0.0f); }

// Boom axis from its two angles, per the physics kinematics (sim frame).
XMVECTOR BoomDirection(float yaw, float pitch)
{
    return XMVectorSet(-std::cos(yaw) * std::cos(pitch),
                        std::sin(yaw) * std::cos(pitch),
                       -std::sin(pitch), 0.0f);
}

}  // namespace

namespace {

// Uniform Catmull-Rom through four points; b and c are the segment ends, a and d the neighbours.
XMVECTOR CatmullRom4(XMVECTOR a, XMVECTOR b, XMVECTOR c, XMVECTOR d, float f)
{
    const float f2 = f * f, f3 = f2 * f;
    XMVECTOR r = XMVectorScale(b, 2.0f);
    r = XMVectorAdd(r, XMVectorScale(XMVectorAdd(XMVectorNegate(a), c), f));
    r = XMVectorAdd(r, XMVectorScale(
        XMVectorAdd(XMVectorSubtract(XMVectorScale(a, 2.0f), XMVectorScale(b, 5.0f)),
                    XMVectorSubtract(XMVectorScale(c, 4.0f), d)), f2));
    r = XMVectorAdd(r, XMVectorScale(
        XMVectorAdd(XMVectorAdd(XMVectorNegate(a), XMVectorScale(b, 3.0f)),
                    XMVectorAdd(XMVectorScale(c, -3.0f), d)), f3));
    return XMVectorScale(r, 0.5f);
}

// Bicubic (tensor-product Catmull-Rom) sample at (u,v) over a rows x cols lattice.
XMVECTOR PatchEval(const std::vector<XMFLOAT3>& ctrl, int rows, int cols, float u, float v)
{
    const float fv = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(cols - 1);
    const int   j  = std::min(static_cast<int>(fv), cols - 2);
    const float gv = fv - static_cast<float>(j);
    const int j0 = std::max(j - 1, 0), j1 = j, j2 = j + 1, j3 = std::min(j + 2, cols - 1);

    const float fu = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(rows - 1);
    const int   i  = std::min(static_cast<int>(fu), rows - 2);
    const float gu = fu - static_cast<float>(i);
    const int i0 = std::max(i - 1, 0), i1 = i, i2 = i + 1, i3 = std::min(i + 2, rows - 1);

    auto row = [&](int ri) {
        return CatmullRom4(XMLoadFloat3(&ctrl[ri * cols + j0]), XMLoadFloat3(&ctrl[ri * cols + j1]),
                           XMLoadFloat3(&ctrl[ri * cols + j2]), XMLoadFloat3(&ctrl[ri * cols + j3]), gv);
    };
    return CatmullRom4(row(i0), row(i1), row(i2), row(i3), gu);
}

// Two-arc camber line: the mean-line height at chord fraction x for a max camber `depth` sited at
// `position`. Mirrors Sim.Core.Rig.CamberSection.CamberLine so the drawn sail is the shape the
// physics is solving, not a lookalike curve.
float CamberLine(float x, float depth, float position)
{
    if (depth <= 1e-6f) return 0.0f;
    const float p  = position;
    const float rf = (p * p + depth * depth) / (2.0f * depth);
    const float rb = ((1.0f - p) * (1.0f - p) + depth * depth) / (2.0f * depth);
    const float dx = x - p;
    return x <= p ? (depth - rf) + std::sqrt(std::max(rf * rf - dx * dx, 0.0f))
                  : (depth - rb) + std::sqrt(std::max(rb * rb - dx * dx, 0.0f));
}

}  // namespace

// Mast tube: sides per ring and the spar radius. Six reads round enough at the distance a chase
// camera keeps, and costs 12 vertices per segment.
namespace { constexpr int kMastSides = 6; constexpr float kMastRadius = 0.035f; }

// ---- The water the sim samples -----------------------------------------------------------------
// The FFT ocean is the ground truth for BOTH ends: it is drawn here and sampled by the physics
// through SimSessionDesc.host_waves, so the hull cannot float on a surface the player is not looking
// at. What crosses the ABI is elevation and slope; see SimHostWaveField in the submodule's
// SimClient.h for what the sim derives from them and what that costs.

// Refill the boat-centred surface cache at the water clock. Once a frame, before the sim steps, so
// every query inside that step reads one coherent instant.
// Point the wave spectrum down the breeze the sim is actually blowing. Without this the ocean runs
// wherever the water panel was last left, which puts the sea across the wind and reads as subtly
// wrong long before anyone can say why.
//
// Deliberately the MEAN wind and not the sampled field: windTheta sits in PhillipsParams, so every
// change re-runs the Phillips pass for all three cascades. The mean breeze holds still while gusts
// move over it; keying this to the sampled field would rebuild the spectrum every single frame.
void App::FollowSimWind()
{
    if (!m_waterFollowsSimWind) return;

    float wind[2] = {0.0f, 0.0f};
    if (!m_sim.GetMeanWind(wind)) return;
    if (wind[0] * wind[0] + wind[1] * wind[1] < 1e-6f) return;   // becalmed: leave the sea as it is

    // windTheta is "the direction the wind blows TOWARD", which is what a velocity vector already
    // says - no meteorological flip here. x is x; the water's second axis is the renderer's z.
    const float theta = std::atan2(wind[1], wind[0]);

    // A dead band, because the comparison behind the Phillips rebuild is an exact float equality and
    // a hair of drift in the mean would otherwise regenerate three cascades every frame. Half a
    // degree is far below what anyone can see in a wave train.
    auto& tweaks = m_scene.GetWaterSurface().tweaks;
    float delta = theta - tweaks.windTheta;
    while (delta >  3.14159265f) delta -= 6.2831853f;
    while (delta < -3.14159265f) delta += 6.2831853f;
    if (std::fabs(delta) > 0.0087f) tweaks.windTheta = theta;
}

void App::RefreshWaveCache()
{
    if (m_waveCache.size() != static_cast<size_t>(kWaveCacheN) * kWaveCacheN)
        m_waveCache.assign(static_cast<size_t>(kWaveCacheN) * kWaveCacheN, 0.0f);

    // Centred on the boat, because that is where the hull, foils and crew are sampled. Marks and
    // flotsam further out read the clamped edge - see SampleWavesForSim.
    const float half = kWaveCacheSide * 0.5f;
    m_waveCacheOriginX = m_boatPos[0] - half;
    m_waveCacheOriginZ = m_boatPos[2] - half;

    const float cell = kWaveCacheSide / static_cast<float>(kWaveCacheN - 1);

    const auto t0 = std::chrono::steady_clock::now();

    // Read the GPU's own height texels when they are available. This is what finally makes the
    // surface the hull floats on and the surface on screen the SAME surface rather than two
    // evaluations of one spectrum: the CPU series is truncated and does not converge, so no mode
    // count reconciles them - measured, the residual is a randomly-signed 0.05-0.15 m and its ratio
    // to the CPU value is noise, which rules out a scale factor and leaves the missing bands.
    //
    // The readback trails by a few frames. Against wave periods of seconds that lag is invisible,
    // and it buys exactness that no amount of CPU summing can.
    auto& water = m_scene.GetWaterSurface();
    water.EnableHeightReadback(true);

    bool fromGpu = false;
    {
        float probe = 0.0f;
        fromGpu = water.SampleHeightGPU(m_waveCacheOriginX, m_waveCacheOriginZ, probe);
    }

    if (fromGpu)
    {
        for (int j = 0; j < kWaveCacheN; ++j)
            for (int i = 0; i < kWaveCacheN; ++i)
            {
                float h = 0.0f;
                water.SampleHeightGPU(m_waveCacheOriginX + i * cell,
                                      m_waveCacheOriginZ + j * cell, h);
                m_waveCache[static_cast<size_t>(j) * kWaveCacheN + i] = h;
            }
    }
    else
    {
        // Until the first capture lands, fall back to the truncated CPU series - a few frames of
        // approximate water at startup beats a few frames of flat water.
        water.SampleHeightGrid(m_waveCacheOriginX, m_waveCacheOriginZ, cell, kWaveCacheN,
                               m_waterTime, kWaveCacheModes, m_waveCache.data());
    }
    m_waveCacheFromGpu = fromGpu;
    m_waveCacheMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    m_waveCacheValid = true;

}

void App::SampleWavesForSim(void* user, const float* xy, int count, float /*t*/,
                            float* outElevation, float* outSlope)
{
    auto* app = static_cast<App*>(user);
    const float cell = kWaveCacheSide / static_cast<float>(kWaveCacheN - 1);

    // The time the sim passes is deliberately ignored. The cache holds one instant per frame, and the
    // ~7 steps inside a frame span 17 ms of a swell whose period is seconds; rebuilding per step would
    // cost far more than that error is worth. It does mean the physics samples the surface at the
    // frame's clock rather than the step's, which is the same approximation the old bridge made.
    auto height = [app, cell](float wx, float wz) -> float
    {
        if (!app->m_waveCacheValid) return 0.0f;

        // Clamped, not wrapped: outside the cache the honest answer is "the nearest water I have".
        // Wrapping would put a mark on a crest that belongs 48 m away, and the alternative - evaluating
        // the spectrum directly out here - is the per-query cost this cache exists to avoid.
        const float gx = std::clamp((wx - app->m_waveCacheOriginX) / cell, 0.0f,
                                    static_cast<float>(kWaveCacheN - 1));
        const float gz = std::clamp((wz - app->m_waveCacheOriginZ) / cell, 0.0f,
                                    static_cast<float>(kWaveCacheN - 1));

        const int   x0 = static_cast<int>(gx), z0 = static_cast<int>(gz);
        const int   x1 = std::min(x0 + 1, kWaveCacheN - 1);
        const int   z1 = std::min(z0 + 1, kWaveCacheN - 1);
        const float fx = gx - static_cast<float>(x0), fz = gz - static_cast<float>(z0);

        const auto& h = app->m_waveCache;
        const float h00 = h[static_cast<size_t>(z0) * kWaveCacheN + x0];
        const float h10 = h[static_cast<size_t>(z0) * kWaveCacheN + x1];
        const float h01 = h[static_cast<size_t>(z1) * kWaveCacheN + x0];
        const float h11 = h[static_cast<size_t>(z1) * kWaveCacheN + x1];
        return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz)
             + (h01 * (1.0f - fx) + h11 * fx) * fz;
    };

    for (int i = 0; i < count; ++i)
    {
        // The sim's horizontal plane is (x, y) with +z up; this renderer's is (x, z) with +y up. The
        // two horizontal axes map straight through, so a sim xy is a renderer xz and the elevation it
        // wants is the renderer's height.
        const float wx = xy[i * 2 + 0];
        const float wz = xy[i * 2 + 1];

        outElevation[i] = height(wx, wz);

        if (outSlope)
        {
            // Central difference over one cell. Differencing the CACHE rather than the spectrum keeps
            // slope consistent with the elevation beside it: a slope taken from the true surface while
            // the height came from a bilinear patch would disagree at every cell boundary, and the
            // hull would feel a facet the water does not have.
            const float e = cell;
            outSlope[i * 2 + 0] = (height(wx + e, wz) - height(wx - e, wz)) / (2.0f * e);
            outSlope[i * 2 + 1] = (height(wx, wz + e) - height(wx, wz - e)) / (2.0f * e);
        }
    }
}

// ---- The sailboat ------------------------------------------------------------------------------
// The sim is authoritative for the boat and reaches nothing in here: this file consumes its state
// and produces an input frame, which is the whole contract (engine-integration.md in the submodule).

// Author the session, load the hull, and fix the rig mesh's topology once.
void App::InitSim()
{
    // Calm water on purpose. The sim owns a seeded wave spectrum and expects a renderer to evaluate
    // the same components; this renderer draws an FFT ocean, which is different maths, so a non-zero
    // sea state would float the hull on a surface the player cannot see. At 0 the sim publishes an
    // empty component list - flat water - and the hull provably sits on it.
    // The last two arguments are the whole inversion: the sim samples THIS surface instead of
    // building its own, so m_seaState no longer describes the sea - the FFT ocean does.
    if (!m_sim.BeginLocal(m_seed, m_windSpeed, XMConvertToRadians(m_windFromDeg),
                          m_seaState, "", 1, &App::SampleWavesForSim, this))
    {
        fprintf(stderr, "[boat] no sim session; the hull will not be drawn.\n");
        return;
    }

    // The hull is the RENDERER's asset: the ABI exports no geometry, it exports a class_id and says
    // to resolve a mesh by it. models/laser-hull.obj is generated from the sim's own hull by
    // tools/hull-export, already in this renderer's frame.
    std::vector<float> pos, nrm;
    std::vector<uint32_t> idx;
    if (!LoadObj(ResolveExePath("models/laser-hull.obj"), pos, nrm, idx))
    {
        fprintf(stderr, "[boat] models/laser-hull.obj missing - run tools/hull-export.\n");
        return;
    }

    auto hullMat = m_renderer->GetResourceManager().CreateMaterial("boat/hull");
    hullMat->baseColorFactor = {0.92f, 0.92f, 0.94f, 1.0f};
    hullMat->roughness       = 0.35f;
    hullMat->metallic        = 0.0f;
    hullMat->doubleSided     = false;
    hullMat->shadingModel    = ShadingModel::PBR;
    hullMat->CreatePSO(m_device.Get());

    auto hull = std::make_shared<Mesh>();
    hull->CreateFromArrays(pos.data(), nrm.data(), static_cast<uint32_t>(pos.size() / 3),
                           idx.data(), static_cast<uint32_t>(idx.size()), hullMat, m_device.Get());

    Entity boat;
    boat.mesh    = hull;
    boat.visible = true;
    XMStoreFloat4x4(&boat.worldTransform, XMMatrixIdentity());
    m_scene.GetEntities().push_back(boat);
    m_boatEntity = static_cast<int>(m_scene.GetEntities().size()) - 1;

    // Sail and spar share the rig's dynamic mesh, one submesh each.
    m_sailMat = m_renderer->GetResourceManager().CreateMaterial("boat/sail");
    m_sailMat->baseColorFactor = {0.96f, 0.96f, 0.93f, 1.0f};
    m_sailMat->roughness       = 0.55f;
    m_sailMat->metallic        = 0.0f;
    m_sailMat->doubleSided     = true;   // cloth is seen from both sides
    m_sailMat->shadingModel    = ShadingModel::PBR;
    m_sailMat->CreatePSO(m_device.Get());

    m_sparMat = m_renderer->GetResourceManager().CreateMaterial("boat/spar");
    m_sparMat->baseColorFactor = {0.18f, 0.19f, 0.21f, 1.0f};
    m_sparMat->roughness       = 0.4f;
    m_sparMat->metallic        = 0.6f;
    m_sparMat->doubleSided     = true;
    m_sparMat->shadingModel    = ShadingModel::PBR;
    m_sparMat->CreatePSO(m_device.Get());

    auto makeRigMat = [&](const char* key, XMFLOAT4 colour, float rough, float metal) {
        auto m = m_renderer->GetResourceManager().CreateMaterial(key);
        m->baseColorFactor = colour;
        m->roughness    = rough;
        m->metallic     = metal;
        m->doubleSided  = true;   // the sim->render mirror flips winding; see UpdateRigMesh
        m->shadingModel = ShadingModel::PBR;
        m->CreatePSO(m_device.Get());
        return m;
    };
    m_lineMat     = makeRigMat("boat/line",     {0.90f, 0.86f, 0.74f, 1.0f}, 0.75f, 0.0f);
    m_foilMat     = makeRigMat("boat/foil",     {0.82f, 0.84f, 0.86f, 1.0f}, 0.30f, 0.1f);
    m_markerMat   = makeRigMat("boat/marker",   {0.20f, 0.85f, 0.45f, 1.0f}, 0.6f,  0.0f);
    m_telltaleMat = makeRigMat("boat/telltale", {0.90f, 0.15f, 0.15f, 1.0f}, 0.7f,  0.0f);

    // Fill the surface before the first step, or that step samples an empty cache and the boat
    // starts life underwater.
    RefreshWaveCache();

    // Run one frame of the sim so the view is populated: panel count is static per class cut, but
    // the panels themselves only exist after the first sim_view_update, and the mast likewise.
    m_sim.Update(1.0f / 60.0f);
    m_sailPanelCount = static_cast<int>(m_sim.Panels().size());
    const int mastPts = static_cast<int>(m_sim.MastPoints().size() / 3);
    m_mastSegments = mastPts > 1 ? mastPts - 1 : 0;

    if (m_sailPanelCount == 0 && m_mastSegments == 0)
    {
        fprintf(stderr, "[boat] sim produced no rig geometry; hull only.\n");
        return;
    }

    BuildRigTopology();

    fprintf(stderr, "[boat] hull %zu tris, %d sail panels -> %dx%d patch, %d mast segments.\n",
            idx.size() / 3, m_sailPanelCount, kSailRows, kSailCols, m_mastSegments);
}

// Sim pose -> the boat entity's world transform. The pose is already in the renderer's frame, and
// so is the hull mesh (tools/hull-export bakes the basis change), so this is just compose-and-store.
// The rig mesh's fixed topology. Split out of InitSim because a boat reset re-authors the session,
// and the panel and mast counts are only knowable after the new session has produced a view.
void App::BuildRigTopology()
{
    // Fixed topology, set once. The sail grid is a constant size regardless of how many panels
    // the class cut publishes - the patch absorbs that - so nothing here has to be resized when a
    // boat of a different class turns up.
    auto& rig = m_scene.GetRig();
    const int sailVerts = kSailRows * kSailCols;
    const int mastVerts = m_mastSegments * kMastSides * 2;
    const int tubeVerts = kMastSides * 2;    // one tube: boom, each line, each telltale

    // Layout: sail | mast | boom | lines | foils | markers | telltales. Held as explicit bases
    // because the mast segment count is only known after the first view update, so nothing
    // downstream of it has a compile-time offset.
    m_boomBase     = sailVerts + mastVerts;
    m_lineBase     = m_boomBase + tubeVerts;
    m_foilBase     = m_lineBase + kLineTubes * tubeVerts;
    m_markerBase   = m_foilBase + 2 * kBladeVerts;
    m_telltaleBase = m_markerBase + kSphereVerts + kFlyVerts;
    const int totalVerts = m_telltaleBase + kTelltaleCount * tubeVerts;

    rig.vertices.assign(static_cast<size_t>(totalVerts), PbrVertex{});
    rig.indices.clear();

    for (int r = 0; r + 1 < kSailRows; ++r)
        for (int k = 0; k + 1 < kSailCols; ++k)
        {
            const uint32_t a = static_cast<uint32_t>(r * kSailCols + k);
            const uint32_t b = a + 1;
            const uint32_t c = a + kSailCols;
            const uint32_t d = c + 1;
            rig.indices.insert(rig.indices.end(), {a, c, d, a, d, b});
        }
    const uint32_t sailIndexCount = static_cast<uint32_t>(rig.indices.size());

    // A tube is two rings of kMastSides; quads between them.
    auto tubeIndices = [&](int baseVertex) {
        for (int side = 0; side < kMastSides; ++side)
        {
            const uint32_t a0 = static_cast<uint32_t>(baseVertex + side);
            const uint32_t a1 = static_cast<uint32_t>(baseVertex + (side + 1) % kMastSides);
            const uint32_t b0 = a0 + kMastSides, b1 = a1 + kMastSides;
            rig.indices.insert(rig.indices.end(), {a0, b0, b1, a0, b1, a1});
        }
    };

    for (int seg = 0; seg < m_mastSegments; ++seg) tubeIndices(sailVerts + seg * tubeVerts);
    tubeIndices(m_boomBase);
    const uint32_t sparIndexCount = static_cast<uint32_t>(rig.indices.size()) - sailIndexCount;

    for (int i = 0; i < kLineTubes; ++i) tubeIndices(m_lineBase + i * tubeVerts);
    const uint32_t lineIndexCount =
        static_cast<uint32_t>(rig.indices.size()) - sailIndexCount - sparIndexCount;

    // A blade is a tapered box: 0-3 on one face, 4-7 on the other.
    for (int b = 0; b < 2; ++b)
    {
        const uint32_t v = static_cast<uint32_t>(m_foilBase + b * kBladeVerts);
        const uint32_t face[6][4] = {
            {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 5, 1, 0}, {3, 2, 6, 7}, {4, 0, 3, 7}, {1, 5, 6, 2},
        };
        for (const auto& f : face)
            rig.indices.insert(rig.indices.end(),
                               {v + f[0], v + f[1], v + f[2], v + f[0], v + f[2], v + f[3]});
    }
    const uint32_t foilIndexCount = static_cast<uint32_t>(rig.indices.size())
                                    - sailIndexCount - sparIndexCount - lineIndexCount;

    // Markers: a UV sphere for the crew, then the flat masthead-fly arrow.
    for (int i = 0; i < kSphereStacks; ++i)
        for (int j = 0; j < kSphereSlices; ++j)
        {
            const uint32_t a = static_cast<uint32_t>(m_markerBase + i * (kSphereSlices + 1) + j);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>(m_markerBase + (i + 1) * (kSphereSlices + 1) + j);
            const uint32_t d = c + 1;
            rig.indices.insert(rig.indices.end(), {a, c, d, a, d, b});
        }
    {
        const uint32_t f = static_cast<uint32_t>(m_markerBase + kSphereVerts);
        rig.indices.insert(rig.indices.end(),
                           {f + 0, f + 1, f + 2, f + 0, f + 2, f + 3,    // shaft
                            f + 4, f + 5, f + 6});                        // arrowhead
    }
    const uint32_t markerIndexCount = static_cast<uint32_t>(rig.indices.size())
        - sailIndexCount - sparIndexCount - lineIndexCount - foilIndexCount;

    for (int k = 0; k < kTelltaleCount; ++k) tubeIndices(m_telltaleBase + k * tubeVerts);
    const uint32_t telltaleIndexCount = static_cast<uint32_t>(rig.indices.size())
        - sailIndexCount - sparIndexCount - lineIndexCount - foilIndexCount - markerIndexCount;

    rig.submeshes.clear();
    uint32_t at = 0;
    auto addPart = [&](uint32_t count, const std::shared_ptr<Material>& mat) {
        if (count > 0) rig.submeshes.push_back({at, count, mat});
        at += count;
    };
    addPart(sailIndexCount,     m_sailMat);
    addPart(sparIndexCount,     m_sparMat);
    addPart(lineIndexCount,     m_lineMat);
    addPart(foilIndexCount,     m_foilMat);
    addPart(markerIndexCount,   m_markerMat);
    addPart(telltaleIndexCount, m_telltaleMat);
    rig.visible = true;
}

// Put the boat back where it started. The session is re-authored rather than rewound - the ABI has
// no reset - so everything on this side that outlived it has to go back too, or the new boat inherits
// the old one's trim, the old sail shape easing away, and a camera still swung to where it was.
void App::ResetBoat()
{
    if (m_boatEntity < 0) return;

    if (!m_sim.Restart())
    {
        fprintf(stderr, "[boat] reset failed; the session is gone and the boat will not move.\n");
        return;
    }

    // Step once so the new session has a view: panel and mast counts are only known after that, and
    // the topology below is sized from them.
    RefreshWaveCache();
    m_sim.Update(1.0f / 60.0f);

    const int panels = static_cast<int>(m_sim.Panels().size());
    const int mastPts = static_cast<int>(m_sim.MastPoints().size() / 3);
    const int segs = mastPts > 1 ? mastPts - 1 : 0;
    if (panels != m_sailPanelCount || segs != m_mastSegments)
    {
        // Same class, so this should not move. Rebuilt rather than assumed, because a stale index
        // buffer against a resized vertex array reads off the end.
        m_sailPanelCount = panels;
        m_mastSegments   = segs;
        BuildRigTopology();
    }

    // Trim back to the class defaults - the same values the members are declared with.
    m_sheet = 4.0f; m_vang = 0.866f; m_outhaul = 0.09253f;
    m_helm = 0.0f; m_centreboard = 0.0f; m_traveler = 1.5707963f;
    m_hike = 0.0f; m_foreAft = 0.0f;

    // Smoothed shapes belong to the boat that just went away. Left set, the sail would ease out of
    // its old shape over the next tenth of a second and the boom would swing from wherever it was.
    m_sailSmoothInit = false;
    m_boomSmoothInit = false;

    m_camOrbitYaw = 0.6f; m_camOrbitPitch = 0.0f; m_camZoom = 1.0f;
    m_cameraInitialised = false;

    // The new session starts its phase clock at zero, so the water has to follow it rather than
    // carry on from the old one - otherwise the surface jumps on the frame after a reset.
    m_waterTime = m_sim.PhaseTime();

    // Adopt the new pose now rather than waiting for the frame loop, so nothing draws a frame with
    // the old boat still at the old place.
    UpdateBoatFromSim();
    UpdateRigMesh(0.0f);   // dt 0: snap the sail to its new shape instead of easing out of the old

    fprintf(stderr, "[boat] reset: new session, %d sail panels.\n", panels);
}

void App::UpdateBoatFromSim()
{
    if (m_boatEntity < 0) return;
    if (!m_sim.GetOwnPose(m_boatPos, m_boatQuat)) return;

    const XMVECTOR q = XMVectorSet(m_boatQuat[0], m_boatQuat[1], m_boatQuat[2], m_boatQuat[3]);
    const XMMATRIX world = XMMatrixRotationQuaternion(q) *
                           XMMatrixTranslation(m_boatPos[0], m_boatPos[1], m_boatPos[2]);
    XMStoreFloat4x4(&m_scene.GetEntities()[m_boatEntity].worldTransform, world);
}

// Rebuild the sail surface and mast from this frame's view. Both arrive in the boat's BODY frame, so
// the whole thing is built there and taken out through the boat's world matrix once at the end.
//
// The sail is not the panels. A tensor-product Catmull-Rom patch is fitted through the panel-boundary
// corners spanwise, with the two-arc camber baked in chordwise, then sampled at a fixed grid -- so the
// leech is a curve rather than a chain of facets, and the surface has no crease where panels meet.
void App::UpdateRigMesh(float dt)
{
    auto& rig = m_scene.GetRig();
    if (rig.vertices.empty()) { rig.visible = false; return; }

    const auto& panels = m_sim.Panels();
    const auto& mast   = m_sim.MastPoints();

    const XMVECTOR q = XMVectorSet(m_boatQuat[0], m_boatQuat[1], m_boatQuat[2], m_boatQuat[3]);
    const XMVECTOR t = XMVectorSet(m_boatPos[0], m_boatPos[1], m_boatPos[2], 0.0f);
    auto toWorld = [&](XMVECTOR body) { return XMVectorAdd(XMVector3Rotate(body, q), t); };
    auto load3   = [](const float v[3]) { return XMVectorSet(v[0], v[1], v[2], 0.0f); };
    auto store = [&](size_t at, XMVECTOR p, XMVECTOR n) {
        PbrVertex& v = rig.vertices[at];
        v.position[0] = XMVectorGetX(p); v.position[1] = XMVectorGetY(p); v.position[2] = XMVectorGetZ(p);
        v.normal[0]   = XMVectorGetX(n); v.normal[1]   = XMVectorGetY(n); v.normal[2]   = XMVectorGetZ(n);
        v.uv[0] = v.uv[1] = 0.0f;
    };

    const int n = static_cast<int>(panels.size());
    const size_t sailVerts = static_cast<size_t>(kSailRows) * kSailCols;

    // Zero panels is legal - a boat below NEAR detail carries no sail on the wire - and collapsing the
    // grid is how that draws as nothing rather than as stale cloth.
    if (n == 0)
    {
        for (size_t i = 0; i < sailVerts; ++i) store(i, t, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        m_sailSmoothInit = false;   // next sail starts fresh rather than easing out of a collapsed one
    }
    else
    {
        // ---- the control lattice, in body frame ------------------------------------------------
        // One row per panel BOUNDARY (n + 1 of them), so row i takes the lower edge of panel 0 or the
        // upper edge of panel i-1.
        const int rows = n + 1;
        const int cols = kSailCtrlCols;
        m_sailCtrl.resize(static_cast<size_t>(rows) * cols);

        // Draft depth by span height. The panels publish their own once the rig is loaded; before that
        // they read zero, and a flat sail is better than a guessed one, so it simply stays flat.
        auto depthAt = [&](float h) {
            if (n == 1) return panels[0].draftDepth;
            const float fh = std::clamp(h, 0.0f, 1.0f) * static_cast<float>(n) - 0.5f;
            const int   i0 = std::clamp(static_cast<int>(std::floor(fh)), 0, n - 1);
            const int   i1 = std::clamp(i0 + 1, 0, n - 1);
            const float f  = std::clamp(fh - static_cast<float>(i0), 0.0f, 1.0f);
            return panels[i0].draftDepth + (panels[i1].draftDepth - panels[i0].draftDepth) * f;
        };

        for (int i = 0; i < rows; ++i)
        {
            const SailPanel& edge = panels[std::min(i == 0 ? 0 : i - 1, n - 1)];
            const XMVECTOR luff  = (i == 0) ? load3(edge.luffLower)  : load3(edge.luffUpper);
            const XMVECTOR leech = (i == 0) ? load3(edge.leechLower) : load3(edge.leechUpper);

            const SailPanel& src = panels[std::min(i, n - 1)];
            XMVECTOR normal = load3(src.normal);
            if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-8f)
                normal = XMVector3Cross(XMVectorSubtract(leech, luff), XMVectorSet(0, 1, 0, 0));
            normal = XMVector3Normalize(normal);

            const XMVECTOR chord = XMVectorSubtract(leech, luff);
            const float chordLen = XMVectorGetX(XMVector3Length(chord));
            const float depth    = depthAt(static_cast<float>(i) / static_cast<float>(n));

            for (int j = 0; j < cols; ++j)
            {
                const float x = static_cast<float>(j) / static_cast<float>(cols - 1);
                const XMVECTOR onChord = XMVectorAdd(luff, XMVectorScale(chord, x));
                const float    camber  = CamberLine(x, depth, kCamberPos) * chordLen;
                XMStoreFloat3(&m_sailCtrl[static_cast<size_t>(i) * cols + j],
                              XMVectorAdd(onChord, XMVectorScale(normal, camber)));
            }
        }

        // ---- temporal low-pass, in body frame --------------------------------------------------
        // Framerate-independent, and applied to the SHAPE only: the boat's rigid motion comes from the
        // pose below and is not smoothed, so the hull never lags its own wake while the leech settles.
        const float a = dt > 0.0f ? 1.0f - std::exp(-dt / kSailSmoothTau) : 1.0f;
        if (m_sailCtrlSmooth.size() != m_sailCtrl.size()) { m_sailCtrlSmooth = m_sailCtrl; m_sailSmoothInit = false; }
        for (size_t i = 0; i < m_sailCtrl.size(); ++i)
        {
            if (!m_sailSmoothInit) { m_sailCtrlSmooth[i] = m_sailCtrl[i]; continue; }
            m_sailCtrlSmooth[i].x += (m_sailCtrl[i].x - m_sailCtrlSmooth[i].x) * a;
            m_sailCtrlSmooth[i].y += (m_sailCtrl[i].y - m_sailCtrlSmooth[i].y) * a;
            m_sailCtrlSmooth[i].z += (m_sailCtrl[i].z - m_sailCtrlSmooth[i].z) * a;
        }
        m_sailSmoothInit = true;

        // ---- sample the patch ------------------------------------------------------------------
        // Normals are ANALYTIC - the cross product of the patch tangents by central difference - not
        // an average of face normals, so the shading carries no triangulation banding.
        const float hu = 0.5f / static_cast<float>(std::max(1, kSailRows - 1));
        const float hv = 0.5f / static_cast<float>(std::max(1, kSailCols - 1));
        const XMVECTOR leeward = XMVector3Normalize(load3(panels[n / 2].normal));

        for (int r = 0; r < kSailRows; ++r)
        {
            const float u = static_cast<float>(r) / static_cast<float>(kSailRows - 1);
            for (int k = 0; k < kSailCols; ++k)
            {
                const float v = static_cast<float>(k) / static_cast<float>(kSailCols - 1);
                const XMVECTOR p  = PatchEval(m_sailCtrlSmooth, rows, cols, u, v);
                const XMVECTOR pu = XMVectorSubtract(PatchEval(m_sailCtrlSmooth, rows, cols, u + hu, v),
                                                     PatchEval(m_sailCtrlSmooth, rows, cols, u - hu, v));
                const XMVECTOR pv = XMVectorSubtract(PatchEval(m_sailCtrlSmooth, rows, cols, u, v + hv),
                                                     PatchEval(m_sailCtrlSmooth, rows, cols, u, v - hv));

                XMVECTOR nrm = XMVector3Cross(pu, pv);
                if (XMVectorGetX(XMVector3LengthSq(nrm)) < 1e-12f) nrm = leeward;
                nrm = XMVector3Normalize(nrm);
                // Turned to agree with the leeward direction the sim published, which gives the normal
                // a defined side without having to reason about winding through the mirrored frame.
                // SHADING no longer depends on this: the sail material is two-sided, so the pixel
                // shader faces the normal at the viewer regardless. That matters because the published
                // leeward direction legitimately flips when the boom crosses the centreline - it was
                // doing so several times a second while luffing - and the whole sail used to switch
                // between lit and unlit with it.
                if (XMVectorGetX(XMVector3Dot(nrm, leeward)) < 0.0f) nrm = XMVectorNegate(nrm);

                store(static_cast<size_t>(r) * kSailCols + k, toWorld(p), XMVector3Rotate(nrm, q));
            }
        }
    }

    // ---- mast ----------------------------------------------------------------------------------
    // A thin tube swept along the centreline. Ring axes come from the segment direction rather than a
    // fixed up-vector, so a mast lying near-horizontal in a capsize stays solid.
    const int segs = std::min(m_mastSegments, std::max(0, static_cast<int>(mast.size() / 3) - 1));
    const size_t tubeBase = sailVerts;
    for (int seg = 0; seg < segs; ++seg)
    {
        const XMVECTOR p0 = toWorld(load3(&mast[static_cast<size_t>(seg) * 3]));
        const XMVECTOR p1 = toWorld(load3(&mast[static_cast<size_t>(seg + 1) * 3]));

        XMVECTOR axis = XMVectorSubtract(p1, p0);
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-10f) axis = XMVectorSet(0, 1, 0, 0);
        axis = XMVector3Normalize(axis);

        const XMVECTOR seed = std::fabs(XMVectorGetY(axis)) > 0.9f ? XMVectorSet(1, 0, 0, 0)
                                                                   : XMVectorSet(0, 1, 0, 0);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(axis, seed));
        const XMVECTOR v = XMVector3Cross(axis, u);

        const size_t ring = tubeBase + static_cast<size_t>(seg) * kMastSides * 2;
        for (int side = 0; side < kMastSides; ++side)
        {
            const float a = 6.2831853f * static_cast<float>(side) / static_cast<float>(kMastSides);
            const XMVECTOR radial = XMVectorAdd(XMVectorScale(u, std::cos(a)),
                                                XMVectorScale(v, std::sin(a)));
            const XMVECTOR offset = XMVectorScale(radial, kMastRadius);
            store(ring + static_cast<size_t>(side),             XMVectorAdd(p0, offset), radial);
            store(ring + static_cast<size_t>(side) + kMastSides, XMVectorAdd(p1, offset), radial);
        }
    }
    for (int seg = segs; seg < m_mastSegments; ++seg)
    {
        const size_t ring = tubeBase + static_cast<size_t>(seg) * kMastSides * 2;
        for (int k = 0; k < kMastSides * 2; ++k)
            store(ring + static_cast<size_t>(k), t, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }

    // ---- boom, lines, foils, markers, telltales --------------------------------------------------
    // Everything below is posed in the SIM's body frame, matching the physics kinematics, and crosses
    // into the renderer's frame once per point through SimToRender.
    {
        auto simToWorld = [&](XMVECTOR simBody) { return toWorld(SimToRender(simBody)); };
        auto simDirToWorld = [&](XMVECTOR simDir) { return XMVector3Rotate(SimToRender(simDir), q); };

        // A capped tube between two world points: two rings of kMastSides, radial normals.
        auto writeTube = [&](size_t base, XMVECTOR a, XMVECTOR b, float radius)
        {
            XMVECTOR axis = XMVectorSubtract(b, a);
            if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f) axis = XMVectorSet(0, 1, 0, 0);
            axis = XMVector3Normalize(axis);
            const XMVECTOR seed = std::fabs(XMVectorGetY(axis)) > 0.99f ? XMVectorSet(1, 0, 0, 0)
                                                                        : XMVectorSet(0, 1, 0, 0);
            const XMVECTOR u = XMVector3Normalize(XMVector3Cross(axis, seed));
            const XMVECTOR v = XMVector3Cross(axis, u);
            for (int ring = 0; ring < 2; ++ring)
                for (int side = 0; side < kMastSides; ++side)
                {
                    const float ang = 6.2831853f * static_cast<float>(side) / kMastSides;
                    const XMVECTOR radial = XMVectorAdd(XMVectorScale(u, std::cos(ang)),
                                                        XMVectorScale(v, std::sin(ang)));
                    store(base + static_cast<size_t>(ring) * kMastSides + side,
                          XMVectorAdd(ring == 0 ? a : b, XMVectorScale(radial, radius)), radial);
                }
        };

        // The boom, from the gooseneck along its solved axis. Smoothed with the same low-pass as the
        // sail: the spars and every line hanging off them have to follow the cloth, not lead it.
        const float aSmooth = dt > 0.0f ? 1.0f - std::exp(-dt / kSailSmoothTau) : 1.0f;
        if (!m_boomSmoothInit)
        {
            m_boomYawS = m_sim.BoomYaw(); m_boomPitchS = m_sim.BoomPitch(); m_boomSmoothInit = true;
        }
        else
        {
            m_boomYawS   += (m_sim.BoomYaw()   - m_boomYawS)   * aSmooth;
            m_boomPitchS += (m_sim.BoomPitch() - m_boomPitchS) * aSmooth;
        }

        const XMVECTOR boomDir = BoomDirection(m_boomYawS, m_boomPitchS);
        const XMVECTOR goose   = SimConst(kGooseneck_S);
        const XMVECTOR boomTip = XMVectorAdd(goose, XMVectorScale(boomDir, kBoomLength_m));
        writeTube(static_cast<size_t>(m_boomBase), simToWorld(goose), simToWorld(boomTip), 0.03f);

        // Lines. The sheet runs from the hull block to the bail near the clew; the bridle carries the
        // block between two transom anchors; the vang braces the boom down to the mast step.
        const XMVECTOR bail = XMVectorAdd(goose,
            XMVectorScale(boomDir, kBoomLength_m - kBailInboard_m));
        const XMVECTOR vangBoom = XMVectorAdd(goose, XMVectorScale(boomDir, kVangOnBoom_m));
        const XMVECTOR block = SimConst(kHullBlock_S);

        size_t line = static_cast<size_t>(m_lineBase);
        writeTube(line, simToWorld(block), simToWorld(bail), 0.014f);
        line += kMastSides * 2;
        writeTube(line, simToWorld(SimConst(kBridlePort_S)), simToWorld(block), 0.012f);
        line += kMastSides * 2;
        writeTube(line, simToWorld(SimConst(kBridleStbd_S)), simToWorld(block), 0.012f);
        line += kMastSides * 2;
        writeTube(line, simToWorld(SimConst(kVangHull_S)), simToWorld(vangBoom), 0.012f);
        line += kMastSides * 2;

        // Forward lead: bail -> gooseneck block -> deck ratchet. Drawn slightly under the boom so
        // it reads as rope hanging beneath the spar rather than buried inside it.
        const XMVECTOR gooseBlock = SimConst(kSheetGooseBlock_S);
        writeTube(line, simToWorld(bail), simToWorld(gooseBlock), 0.012f);
        line += kMastSides * 2;
        writeTube(line, simToWorld(gooseBlock), simToWorld(SimConst(kSheetDeckBlock_S)), 0.012f);

        // Foils: a tapered thin box each. Directions follow the physics kinematics - the keel at a
        // fixed rake, the rudder raked and then turned about its own stock by the ACHIEVED blade
        // angle rather than the helm command, so a blade still catching up is drawn where it is.
        auto writeBlade = [&](size_t base, XMVECTOR root, XMVECTOR spanDir, XMVECTOR chordDir,
                              float span, float rootChord, float tipChord, float thickness)
        {
            const XMVECTOR thick = XMVector3Normalize(XMVector3Cross(spanDir, chordDir));
            const XMVECTOR tip = XMVectorAdd(root, XMVectorScale(spanDir, span));
            const float rc = rootChord * 0.5f, tc = tipChord * 0.5f, ht = thickness * 0.5f;
            const XMVECTOR rLE = XMVectorSubtract(root, XMVectorScale(chordDir, rc));
            const XMVECTOR rTE = XMVectorAdd(root, XMVectorScale(chordDir, rc));
            const XMVECTOR tLE = XMVectorSubtract(tip, XMVectorScale(chordDir, tc));
            const XMVECTOR tTE = XMVectorAdd(tip, XMVectorScale(chordDir, tc));
            const XMVECTOR off = XMVectorScale(thick, ht);
            const XMVECTOR corner[8] = {
                XMVectorAdd(rLE, off), XMVectorAdd(rTE, off), XMVectorAdd(tTE, off), XMVectorAdd(tLE, off),
                XMVectorSubtract(rLE, off), XMVectorSubtract(rTE, off),
                XMVectorSubtract(tTE, off), XMVectorSubtract(tLE, off),
            };
            const XMVECTOR nW = XMVector3Normalize(simDirToWorld(thick));
            for (int i = 0; i < 8; ++i)
                store(base + static_cast<size_t>(i), simToWorld(corner[i]),
                      i < 4 ? nW : XMVectorNegate(nW));
        };

        const XMVECTOR keelSpan  = XMVectorSet(-std::sin(kKeelRake), 0.0f, -std::cos(kKeelRake), 0.0f);
        const XMVECTOR keelChord = XMVectorSet(-std::cos(kKeelRake), 0.0f,  std::sin(kKeelRake), 0.0f);
        writeBlade(static_cast<size_t>(m_foilBase), SimConst(kKeelRoot_S), keelSpan, keelChord,
                   kKeelSpan, kKeelRootChord, kKeelTipChord, kKeelThick);

        const XMVECTOR rudderSpan = XMVectorSet(-std::sin(kRudderRake), 0.0f, -std::cos(kRudderRake), 0.0f);
        const XMVECTOR qRake = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), kRudderRake);
        const XMVECTOR qHelm = XMQuaternionRotationAxis(rudderSpan, m_sim.RudderAngle());
        const XMVECTOR rudderChord = XMVector3Rotate(XMVectorSet(-1, 0, 0, 0),
                                                     XMQuaternionMultiply(qRake, qHelm));
        writeBlade(static_cast<size_t>(m_foilBase) + kBladeVerts, SimConst(kRudderStock_S),
                   rudderSpan, rudderChord, kRudderSpan, kRudderRootChord, kRudderTipChord, kRudderThick);

        // ---- markers -----------------------------------------------------------------------------
        // Crew centre of mass. The ABI publishes no crew state, so this is DERIVED from the hike and
        // fore/aft commands rather than read - it shows where the crew was told to be, which is what
        // the player is steering, but it will not show the sim disagreeing.
        const XMVECTOR crew = XMVectorSet(-0.8f + m_foreAft * 0.35f, m_hike * 0.45f, 0.30f, 0.0f);
        const XMVECTOR crewW = simToWorld(crew);
        size_t vi = static_cast<size_t>(m_markerBase);
        for (int i = 0; i <= kSphereStacks; ++i)
        {
            const float theta = 3.14159265f * static_cast<float>(i) / kSphereStacks;
            const float st = std::sin(theta), ct = std::cos(theta);
            for (int j = 0; j <= kSphereSlices; ++j)
            {
                const float phi = 6.2831853f * static_cast<float>(j) / kSphereSlices;
                const XMVECTOR radial = XMVectorSet(st * std::cos(phi), ct, st * std::sin(phi), 0.0f);
                store(vi++, XMVectorAdd(crewW, XMVectorScale(radial, kCrewMarkerRadius)), radial);
            }
        }

        // Masthead fly: a flat arrow above the masthead pointing INTO the apparent wind, which is what
        // a real vane does - keyed to true wind it would ignore the boat's own motion and lie on any
        // reach. Apparent = ambient wind minus boat velocity.
        const XMVECTOR mastTopW = simToWorld(
            XMVectorAdd(SimConst(kMastBase_S), XMVectorSet(0.0f, 0.0f, kMastHeight_m + 0.12f, 0.0f)));
        float boatVel[3] = {0.0f, 0.0f, 0.0f};
        m_sim.GetVelocity(boatVel);
        const float here[2] = { m_boatPos[0], m_boatPos[2] };
        float windUV[2] = {0.0f, 0.0f};
        m_sim.SampleWind(here, 1, windUV);

        XMVECTOR apparent = XMVectorSet(windUV[0] - boatVel[0], 0.0f, windUV[1] - boatVel[2], 0.0f);
        XMVECTOR flyDir = XMVectorNegate(apparent);   // the vane points at where the wind comes FROM
        if (XMVectorGetX(XMVector3LengthSq(flyDir)) < 1e-8f)
            flyDir = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);   // becalmed: lie along the boat
        flyDir = XMVector3Normalize(flyDir);
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR flyN = XMVector3Normalize(XMVector3Cross(flyDir, up));
        for (int k = 0; k < kFlyVerts; ++k)
            store(vi++, XMVectorAdd(mastTopW,
                                    XMVectorAdd(XMVectorScale(flyDir, kFlyOutline[k][0]),
                                                XMVectorScale(up, kFlyOutline[k][1]))), flyN);

        // ---- leech telltales -----------------------------------------------------------------------
        // A streamer on the leech at four heights, posed by its panel's airflow mode. The severity ramp
        // is the renderer's own cosmetic call - the header says as much - but the SENSE is not: only
        // STALLED and BACKED hook, because a hooked leech telltale is what OVER-trimming looks like.
        // Under-trimmed, the sail is merely unloaded and the trailing edge still streams.
        for (int k = 0; k < kTelltaleCount; ++k)
        {
            const size_t base = static_cast<size_t>(m_telltaleBase) + static_cast<size_t>(k) * kMastSides * 2;
            if (n == 0) { for (int i = 0; i < kMastSides * 2; ++i) store(base + i, t, up); continue; }

            // Spread up the span, skipping the foot panel where a telltale would foul the boom.
            const int pi = std::clamp(static_cast<int>((0.15f + 0.75f * k / std::max(1, kTelltaleCount - 1))
                                                       * static_cast<float>(n)), 0, n - 1);
            const SailPanel& pan = panels[pi];

            const XMVECTOR leechMid = XMVectorScale(
                XMVectorAdd(load3(pan.leechLower), load3(pan.leechUpper)), 0.5f);
            const XMVECTOR luffMid = XMVectorScale(
                XMVectorAdd(load3(pan.luffLower), load3(pan.luffUpper)), 0.5f);
            XMVECTOR aft = XMVectorSubtract(leechMid, luffMid);
            aft = XMVectorGetX(XMVector3LengthSq(aft)) > 1e-8f ? XMVector3Normalize(aft)
                                                               : XMVectorSet(1, 0, 0, 0);
            XMVECTOR lee = load3(pan.normal);
            lee = XMVectorGetX(XMVector3LengthSq(lee)) > 1e-8f ? XMVector3Normalize(lee)
                                                               : XMVector3Normalize(XMVector3Cross(up, aft));

            XMVECTOR dir;
            switch (pan.mode)
            {
                case SailStalled:
                case SailBacked:   // hooked: lifted and curled to leeward
                    dir = XMVectorAdd(XMVectorAdd(XMVectorScale(aft, 0.35f), XMVectorScale(up, 0.95f)),
                                      XMVectorScale(lee, 0.30f));
                    break;
                case SailLuffing:  // no flow: it droops
                    dir = XMVectorSubtract(XMVectorScale(aft, 0.25f), up);
                    break;
                default:                 // flying: streams aft
                    dir = XMVectorSubtract(aft, XMVectorScale(up, 0.12f));
                    break;
            }
            dir = XMVector3Normalize(dir);

            const XMVECTOR attach = XMVectorAdd(leechMid, XMVectorScale(lee, 0.012f));
            writeTube(base, toWorld(attach),
                      toWorld(XMVectorAdd(attach, XMVectorScale(dir, kTelltaleLength))),
                      kTelltaleRadius);
        }
    }

    rig.visible = m_boatEntity >= 0;
}

// Keyboard sailing controls. Rates approximate the eased-slack feel of the Godot build; held keys
// ramp a control at a fixed rate. Bounds are the Laser's, from LaserDefaults in the sim.
void App::UpdateSailControls(float dt)
{
    auto& in = m_inputManager;
    const bool shift = in.IsKeyDown(VK_SHIFT);

    // Mainsheet: S trims in (shorter), Shift+S eases.
    if (in.IsKeyDown('S'))
        m_sheet = shift ? std::min(4.0f, m_sheet + 1.0f * dt)
                        : std::max(0.09f, m_sheet - 1.0f * dt);
    // Vang: V trims, Shift+V eases.
    if (in.IsKeyDown('V'))
        m_vang = shift ? std::min(0.866f, m_vang + 0.3f * dt)
                       : std::max(0.20f, m_vang - 0.3f * dt);
    // Outhaul: O hauls flatter, Shift+O eases.
    if (in.IsKeyDown('O'))
        m_outhaul = shift ? std::min(0.25f, m_outhaul + 0.05f * dt)
                          : std::max(0.0f, m_outhaul - 0.05f * dt);

    // Helm: Left = port, Right = starboard, Space recentres.
    if (in.IsKeyDown(VK_LEFT))  m_helm = std::min(0.5f, m_helm + 1.0f * dt);
    if (in.IsKeyDown(VK_RIGHT)) m_helm = std::max(-0.5f, m_helm - 1.0f * dt);
    if (in.IsKeyDown(VK_SPACE)) m_helm = 0.0f;

    // Crew: , / . hike athwartships, Up / Down move fore and aft. Both persist when released.
    if (in.IsKeyDown(VK_OEM_COMMA))  m_hike = std::min(1.0f, m_hike + 1.5f * dt);
    if (in.IsKeyDown(VK_OEM_PERIOD)) m_hike = std::max(-1.0f, m_hike - 1.5f * dt);
    if (in.IsKeyDown(VK_UP))   m_foreAft = std::min(1.0f, m_foreAft + 1.0f * dt);
    if (in.IsKeyDown(VK_DOWN)) m_foreAft = std::max(-1.0f, m_foreAft - 1.0f * dt);

    // X puts the whole boat back as it started - a new session, so position and motion go too.
    // R resets only the trim and the camera, which is the lighter thing you usually want.
    if (in.IsKeyPressed('X')) ResetBoat();
    // B blows the main (sheet and vang fully eased); R resets the trim and the camera.
    if (in.IsKeyPressed('B')) { m_sheet = 4.0f; m_vang = 0.866f; }
    if (in.IsKeyPressed('R'))
    {
        m_sheet = 4.0f; m_vang = 0.866f; m_outhaul = 0.09253f;
        m_helm = 0.0f; m_centreboard = 0.0f; m_hike = 0.0f; m_foreAft = 0.0f;
        m_camOrbitYaw = 0.6f; m_camOrbitPitch = 0.0f; m_camZoom = 1.0f;
        m_cameraInitialised = false;
    }

    m_sim.SetControls(m_helm, m_sheet, m_vang, m_outhaul,
                      m_centreboard, m_traveler, m_hike, m_foreAft);
}

// Right-mouse orbit and wheel zoom, layered on top of the chase follow.
void App::UpdateCameraInput()
{
    const int wheel = m_inputManager.GetMouseWheelDelta();
    if (wheel != 0)
    {
        const float notches = static_cast<float>(wheel) / 120.0f;   // WHEEL_DELTA
        m_camZoom = std::clamp(m_camZoom * std::pow(1.1f, -notches), 0.3f, 3.0f);
    }
    if (m_inputManager.IsMouseButtonDown(1))
    {
        m_camOrbitYaw -= static_cast<float>(m_inputManager.GetMouseDeltaX()) * 0.005f;
        m_camOrbitPitch = std::clamp(
            m_camOrbitPitch + static_cast<float>(m_inputManager.GetMouseDeltaY()) * 0.005f,
            -1.4f, 1.4f);
    }
    if (m_inputManager.IsKeyPressed('C')) { m_camOrbitYaw = 0.0f; m_camOrbitPitch = 0.0f; }
}

// Spring-arm chase camera: follows the boat's HEADING only - not its heel or pitch, which would
// roll the horizon with every gust - then eases the eye position for framerate-independent lag.
void App::UpdateChaseCamera(float dt)
{
    const XMVECTOR q   = XMVectorSet(m_boatQuat[0], m_boatQuat[1], m_boatQuat[2], m_boatQuat[3]);
    const XMVECTOR pos = XMVectorSet(m_boatPos[0], m_boatPos[1], m_boatPos[2], 0.0f);
    const XMVECTOR up  = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    // Bow direction flattened onto the horizontal plane: this is what drops heel and pitch.
    XMVECTOR fwd = XMVector3Rotate(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), q);
    fwd = XMVectorSetY(fwd, 0.0f);
    if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-6f) fwd = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    fwd = XMVector3Normalize(fwd);

    const XMVECTOR yawQ = XMQuaternionRotationAxis(up, m_camOrbitYaw);
    fwd = XMVector3Rotate(fwd, yawQ);

    // 12 m astern and 4 m up at unit zoom, then pitched about the arm's own right axis.
    XMVECTOR arm = XMVectorAdd(XMVectorScale(fwd, -12.0f * m_camZoom),
                               XMVectorScale(up,    4.0f * m_camZoom));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, fwd));
    arm = XMVector3Rotate(arm, XMQuaternionRotationAxis(right, m_camOrbitPitch));

    const XMVECTOR eye    = XMVectorAdd(pos, arm);
    const XMVECTOR target = XMVectorAdd(pos, XMVectorScale(up, 3.0f));

    XMFLOAT3 eyeF;
    XMStoreFloat3(&eyeF, eye);
    if (!m_cameraInitialised) { m_smoothedCamPos = eyeF; m_cameraInitialised = true; }
    else
    {
        const float a = 1.0f - std::exp(-1.5f * dt);   // ~0.67 s time constant
        m_smoothedCamPos.x += (eyeF.x - m_smoothedCamPos.x) * a;
        m_smoothedCamPos.y += (eyeF.y - m_smoothedCamPos.y) * a;
        m_smoothedCamPos.z += (eyeF.z - m_smoothedCamPos.z) * a;
    }

    XMFLOAT3 targetF;
    XMStoreFloat3(&targetF, target);
    m_camera.SetLookAt(m_smoothedCamPos, targetF);
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

    if (m_inputManager.IsKeyPressed(VK_TAB)) m_followBoat = !m_followBoat;

    // The sim is paused by not stepping it - there is no pause export - which also holds its
    // phase clock, so the water and the hull stay in agreement across a pause.
    m_sim.SetPaused(m_timePaused);

    if (m_followBoat && m_boatEntity >= 0)
    {
        UpdateSailControls(dt);
        UpdateCameraInput();
    }
    else
    {
        m_camera.Update(dt, m_inputManager);
    }

    // Ordering matters: the cache is filled at m_waterTime, the sim then samples that exact
    // surface, and the water is drawn at the same m_waterTime further down. Refreshing after the
    // step would draw one instant and float the hull on another.
    FollowSimWind();
    RefreshWaveCache();
    m_sim.Update(dt);
    m_waterTime = m_sim.PhaseTime();
    UpdateBoatFromSim();
    UpdateRigMesh(dt);
    if (m_followBoat && m_boatEntity >= 0) UpdateChaseCamera(dt);

    if (!m_timePaused)
        m_elapsedTime += dt;   // wall clock: FPS, and the sea when there is no sim to clock it

    // With no session there is no phase clock, so the sea falls back to the wall clock rather
    // than freezing. A pause still holds it either way: the sim stops advancing phase time, and
    // m_elapsedTime stops here.
    if (m_boatEntity < 0) m_waterTime = m_elapsedTime;

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

// ---- Debug windows ----------------------------------------------------------------------------
// One window per concern instead of a single tower of collapsing headers. The sections had grown
// past the point where scrolling to the sail trim while watching the water numbers was practical,
// and ImGui remembers each window's position and size independently.

void App::DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("Windows"))
    {
        ImGui::MenuItem("Sailboat",    nullptr, &m_showSailboatWindow);
        ImGui::MenuItem("Water",       nullptr, &m_showWaterWindow);
        ImGui::Separator();
        ImGui::MenuItem("Stats",       nullptr, &m_showStatsWindow);
        ImGui::MenuItem("GPU Timings", nullptr, &m_showTimingsWindow);
        ImGui::MenuItem("Entities",    nullptr, &m_showEntitiesWindow);
        ImGui::MenuItem("Atmosphere",  nullptr, &m_showAtmosphereWindow);
        ImGui::EndMenu();
    }

    // Frame rate rides the bar itself. It is the one number worth having on screen even when every
    // window is closed, and putting it here means the Stats window can default shut without
    // hiding it.
    char fps[64];
    snprintf(fps, sizeof(fps), "%.1f FPS (%.2f ms)", m_fps, m_fps > 0.0f ? 1000.0f / m_fps : 0.0f);
    const float width = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - width - 16.0f);
    ImGui::TextUnformatted(fps);

    ImGui::EndMainMenuBar();
}

void App::DrawStatsWindow()
{
    if (!m_showStatsWindow) return;
    ImGui::SetNextWindowPos(ImVec2(390.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 150.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Stats", &m_showStatsWindow))
    {
    ImGui::Text("%.1f FPS (%.2f ms)", m_fps, m_fps > 0.0f ? 1000.0f / m_fps : 0.0f);
    XMFLOAT3 pos = m_camera.GetPosition();
    ImGui::Text("Camera: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
    ImGui::SliderFloat("Move Speed", &m_camera.GetMoveSpeed(), 1.0f, 50.0f);
    {
        // Per-frame upload arena. The dynamic rig and the wave-debug overlay both live here, and
        // overflowing it is a hard crash, so the headroom is worth watching rather than discovering.
        const auto& alloc = m_renderer->GetLinearAllocator();
        const double peakKb = static_cast<double>(alloc.PeakBytes()) / 1024.0;
        const double capKb  = static_cast<double>(alloc.PerFrameBytes()) / 1024.0;
        ImGui::Text("Upload arena peak %.0f / %.0f KB", peakKb, capKb);
    }
    ImGui::Checkbox("VSync", &m_vsync);
    ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume (P)" : "Pause (P)"))
        m_timePaused = !m_timePaused;
    }
    ImGui::End();
}

void App::DrawSailboatWindow()
{
    if (!m_showSailboatWindow) return;
    ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sailboat", &m_showSailboatWindow))
    {
    if (m_boatEntity >= 0)
    {
        ImGui::Separator();
        // (the window title says Sailboat; the scope below is just for the locals)
        {
            ImGui::Checkbox("Follow boat (Tab)", &m_followBoat);
            ImGui::Text("Pos %.1f, %.1f, %.1f   panels %d", m_boatPos[0], m_boatPos[1],
                        m_boatPos[2], (int)m_sim.Panels().size());
            ImGui::Text("Phase clock %.2f s   boats %d", m_sim.PhaseTime(), m_sim.BoatCount());

            // Bounds are the Laser's from LaserDefaults, not the wider wire bounds: the wire
            // spans every class, so sliding to its ends would command trim this boat cannot make.
            ImGui::SliderFloat("Sheet (S)",   &m_sheet,   0.09f, 4.0f,   "%.2f m");
            ImGui::SliderFloat("Vang (V)",    &m_vang,    0.20f, 0.866f, "%.3f m");
            ImGui::SliderFloat("Outhaul (O)", &m_outhaul, 0.0f,  0.25f,  "%.3f m");
            ImGui::SliderFloat("Helm",        &m_helm,   -0.5f,  0.5f,   "%.2f rad");
            ImGui::SliderFloat("Centreboard", &m_centreboard, 0.0f, 1.0f, "%.2f up");
            ImGui::SliderFloat("Hike",        &m_hike,   -1.0f,  1.0f);
            ImGui::SliderFloat("Fore/aft",    &m_foreAft,-1.0f,  1.0f);

            if (ImGui::Button("Blow main (B)")) { m_sheet = 4.0f; m_vang = 0.866f; }
            ImGui::SameLine();
            if (ImGui::Button("Reset boat (X)")) ResetBoat();
            ImGui::SameLine();
            if (ImGui::Button("Reset trim (R)"))
            {
                m_sheet = 4.0f; m_vang = 0.866f; m_outhaul = 0.09253f;
                m_helm = 0.0f; m_centreboard = 0.0f; m_hike = 0.0f; m_foreAft = 0.0f;
            }

            // Wind lives in the session desc, read once at begin_local, so it is shown as state
            // rather than offered as a slider that would silently do nothing.
            ImGui::Separator();
            ImGui::Text("Wind %.1f m/s from %.0f deg (set at session start)",
                        m_windSpeed, m_windFromDeg);

            // The reconciliation readout. The sim samples THIS ocean through the ABI, so the water
            // under the hull and the water on screen are the same surface by construction - and this
            // is the number that proves it rather than asserting it. Freeboard is the hull reference
            // point above the local surface: it should hover around a constant and breathe with the
            // swell, not drift away from it.
            const float xy[2] = { m_boatPos[0], m_boatPos[2] };
            float waterY = 0.0f, waterSlope[2] = {0.0f, 0.0f};
            SampleWavesForSim(this, xy, 1, m_waterTime, &waterY, waterSlope);
            ImGui::Text("Water under hull %+.3f m   freeboard %+.3f m", waterY, m_boatPos[1] - waterY);
            ImGui::Text("Surface slope %+.3f, %+.3f   water clock %.2f s",
                        waterSlope[0], waterSlope[1], m_waterTime);
            ImGui::Text("Wave cache %dx%d over %.0f m: %.2f ms  (%s)",
                        kWaveCacheN, kWaveCacheN, kWaveCacheSide, m_waveCacheMs,
                        m_waveCacheFromGpu ? "GPU texels" : "CPU series, warming up");

            // The cache is filled from the GPU texels, so this is not a spectrum comparison any
            // more - it is the cache grid's own interpolation error between exact samples. Kept as a
            // cheap sanity line: if it ever grows past a few millimetres, the two have come apart.
            auto& w = m_scene.GetWaterSurface();
            float gpuH = 0.0f;
            if (w.SampleHeightGPU(m_boatPos[0], m_boatPos[2], gpuH))
            {
                ImGui::Text("GPU %+.4f  cache %+.4f  diff %+.4f m", gpuH, waterY, waterY - gpuH);
            }
            else
            {
                ImGui::TextDisabled("GPU height: waiting for the first readback...");
            }
            ImGui::TextDisabled("The FFT ocean is the sim's water too (host_waves), not just scenery.");
        }
    }
    }
    ImGui::End();
}

void App::DrawEntitiesWindow()
{
    if (!m_showEntitiesWindow) return;
    ImGui::SetNextWindowPos(ImVec2(390.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Entities", &m_showEntitiesWindow))
    {
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
    }
    ImGui::End();
}

void App::DrawWaterWindow()
{
    if (!m_showWaterWindow) return;
    ImGui::SetNextWindowPos(ImVec2(390.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Water", &m_showWaterWindow))
    {
    // (the window title says Water; the scope below is just for the locals)
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
        // Driven from the sim while following, so the slider is disabled rather than left live to be
        // dragged and silently overwritten on the next frame.
        ImGui::Checkbox("Waves follow sim wind", &m_waterFollowsSimWind);
        ImGui::BeginDisabled(m_waterFollowsSimWind);
        ImGui::SliderAngle("Wind Direction",    &tweaks.windTheta,       -180.0f, 180.0f);
        ImGui::EndDisabled();
        ImGui::SliderFloat("Directionality",    &tweaks.directionality,  1.0f, 8.0f, "%.1f");
        ImGui::SliderFloat("Amplitude",         &tweaks.amplitude,       0.0f, 0.25f, "%.3f");
        ImGui::SliderFloat("Small Wave Cutoff", &tweaks.smallWaveCutoff, 0.001f, 0.1f, "%.4f");
        ImGui::SliderFloat("Choppiness",        &tweaks.choppiness,      0.0f,   1.0f);

        // Energy kept in waves running AGAINST the wind. 1 is the classic symmetric Phillips lobe,
        // which lets the sea run upwind as happily as down; 0.07 is Tessendorf's value. Logarithmic,
        // because everything interesting happens in the bottom decade.
        ImGui::SliderFloat("Upwind energy", &tweaks.upwindAttenuation, 0.0f, 1.0f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);

        // Band-limit the RENDERED spectrum. Off (0) is the real ocean; set to the buoyancy sum's
        // mode count and the GPU draws the same truncated surface the physics samples, which is what
        // turns the overlay from "these differ" into "these differ BY MORE THAN truncation".
        ImGui::SliderInt("Mode limit (0 = full)", &tweaks.modeLimit, 0, 64);
        ImGui::SameLine();
        if (ImGui::SmallButton("Match buoyancy")) tweaks.modeLimit = kWaveCacheModes;
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

    }
    }
    ImGui::End();
}

void App::DrawAtmosphereWindow()
{
    if (!m_showAtmosphereWindow) return;
    ImGui::SetNextWindowPos(ImVec2(390.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Atmosphere", &m_showAtmosphereWindow))
    {
    // (the window title says Atmosphere; the scope below is just for the locals)
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
    }
    ImGui::End();
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

    // The water clock, not the wall clock: this is the instant RefreshWaveCache filled the cache
    // at and the sim sampled, so the surface drawn here is the one the hull is sitting on.
    m_renderer->RenderScene(m_scene, view, frameConstants, m_waterTime);

    // ImGui
    m_renderer->GetProfiler().BeginScope("ImGui", m_renderer->GetCommandList());

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawMenuBar();
    DrawStatsWindow();
    DrawSailboatWindow();
    DrawWaterWindow();
    DrawEntitiesWindow();
    DrawAtmosphereWindow();
    // GPU Timings window. Begin and End must pair even when Begin returns false (a collapsed
    // window), so the visibility test guards the whole pair rather than the Begin alone.
    if (m_showTimingsWindow)
    {
    ImGui::SetNextWindowPos(ImVec2(830.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("GPU Timings", &m_showTimingsWindow))
    {
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
    }
    ImGui::End();
    }

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
