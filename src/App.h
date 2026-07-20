#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>
#include "util/FlyCamera.h"
#include "util/InputManager.h"
#include "gfx/ColorBuffer.h"
#include "gfx/DepthBuffer.h"
#include "scene/Scene.h"
#include "SimBridge.h"

class Renderer;
using Microsoft::WRL::ComPtr;

class App
{
public:
    static const UINT FrameCount = 3;
    static const UINT WindowWidth = 1280;
    static const UINT WindowHeight = 720;

    App();
    ~App();

    void Init(HWND hwnd);
    void BeginInputFrame();
    void OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Update(float dt);
    void Render();
    void Cleanup();

private:
    void InitDevice(HWND hwnd);
    void InitSwapChain(HWND hwnd);
    void InitBuffers();
    void InitFence();
    void InitViewport();
    void LoadScene();
    void InitImGui(HWND hwnd);
    void InitSim();
    void UpdateBoatFromSim();
    void UpdateRigMesh(float dt);
    void UpdateDebugWaveMesh();   // fills the CPU wave-sample overlay from the buoyancy cache
    void UpdateSailControls(float dt);
    void UpdateCameraInput();
    void UpdateChaseCamera(float dt);
    void WaitForGpu();
    void WaitForFrame(UINT frameIndex);
    void Resize(UINT width, UINT height);

    UINT m_width  = WindowWidth;
    UINT m_height = WindowHeight;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize = 0;
    ColorBuffer m_displayBuffers[FrameCount];
    DepthBuffer m_depthBuffer;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_nextFenceValue = 1;
    UINT64 m_frameFenceValues[FrameCount] = {};
    HANDLE m_fenceEvent = nullptr;
    UINT m_frameIndex = 0;
    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissorRect = {};

    std::unique_ptr<Renderer> m_renderer;
    InputManager m_inputManager;
    FlyCamera m_camera;
    Scene m_scene;

    // C# physics sim (runs on its own 500 Hz thread; we read its pose each frame).
    SimBridge m_sim;
    int   m_boatEntityIndex = -1;
    float m_boatScale       = 1.0f;   // hull mesh is authored in metres, so 1:1
    bool  m_followBoat      = true;   // chase camera keeps the boat framed
    bool  m_paused          = false;  // freezes physics + ocean-wave time (render/camera stay live)

    // Latest boat pose in the sim's Z-up world frame (for the chase camera).
    float m_boatPos[3]  = {0, 0, 0};
    float m_boatQuat[4] = {0, 0, 0, 1};

    // Dynamic rig mesh scratch (sail + spars + lines + foils), refilled each frame from the bridge.
    int m_rigVertexCount = 0;
    std::vector<float> m_rigPos;
    std::vector<float> m_rigNrm;

    // Editable boat materials (colours tweak baseColorFactor live via ImGui).
    std::shared_ptr<Material> m_hullMat;
    std::shared_ptr<Material> m_sailMat;
    std::shared_ptr<Material> m_foilMat;
    std::shared_ptr<Material> m_markerMat;   // crew-CoM sphere + masthead wind-fly
    std::shared_ptr<Material> m_telltaleMat; // red leech telltales
    std::shared_ptr<Material> m_waveDebugMat; // GPU-match tiles (magenta): what the render shows
    std::shared_ptr<Material> m_physSurfMat;  // physics-surface tiles (green): what buoyancy samples
    bool m_showWaveDebug = false;            // draw CPU-sampled buoyancy heights vs the GPU surface
    std::vector<float> m_dbgH, m_dbgDx, m_dbgDz;  // debug grid: height + horizontal displacement

    // Player controls (sim units). Start fully eased, like the Godot build.
    float m_sheet       = 4.0f;      // BailToBlockDistance_m  [0.09 .. 4.0]  (smaller = trimmed in)
    float m_vang        = 0.866f;    // VangControlLength_m    [0.20 .. 0.866]
    float m_outhaul     = 0.09253f;  // Outhaul_m              [0.0 .. 0.25]
    float m_helm        = 0.0f;      // HelmAngle_rad          [-0.5 .. 0.5]
    float m_centreboard = 0.0f;      // CenterboardRetraction  [0 .. 1]
    float m_hike        = 0.0f;      // HikePosition           [-1 .. 1]  (+ = port)
    float m_foreAft     = 0.0f;      // ForeAftPosition        [-1 .. 1]  (+ = forward)

    // Wind (compass bearing the wind blows toward; 90° = toward +y = from starboard).
    float m_windSpeed  = 4.1f;       // m/s (~8 kn)
    float m_windDirDeg = 90.0f;

    // Chase/orbit camera state (spring-arm, ported from the Godot UpdateChaseCamera).
    float m_camZoom        = 1.0f;   // [0.3 .. 3.0]
    float m_camOrbitYaw    = 0.0f;
    float m_camOrbitPitch  = 0.0f;
    DirectX::XMFLOAT3 m_smoothedCamPos = {0, 0, 0};
    bool  m_cameraInitialised = false;

    float m_telemetry[12] = {};      // HUD snapshot (see SimHost.GetTelemetry layout)

    float m_fps = 0.0f;
    float m_fpsAccumulator = 0.0f;
    int m_fpsFrameCount = 0;
    float m_elapsedTime = 0.0f;
    bool m_vsync = false;

    float m_sunElevation = 45.0f;  // degrees above horizon
    float m_sunAzimuth   = 180.0f; // degrees
};
