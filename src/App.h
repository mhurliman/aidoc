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
    void InitSim();            // author the sim session, load the hull, size the rig mesh
    void BuildRigTopology();   // fixed rig index buffer + submeshes; re-run on a reset
    void ResetBoat();          // re-author the session, putting the boat back as it started
    void UpdateBoatFromSim();  // sim pose -> the boat entity's world transform
    void UpdateRigMesh(float dt);  // rebuild the sail surface + mast from this frame's view
    void UpdateSailControls(float dt);  // keyboard sailing input -> the sim
    void UpdateCameraInput();  // right-mouse orbit, wheel zoom
    void UpdateChaseCamera(float dt);
    void FollowSimWind();      // point the wave spectrum down the breeze the sim is blowing
    void RefreshWaveCache();   // boat-centred CPU surface the sim samples through the ABI

    // The sim asking this renderer for its water. Static because it crosses a C ABI; `user` is
    // the App. Reads the cache RefreshWaveCache filled earlier this frame and never evaluates the
    // spectrum itself - it runs hundreds of times per tick at 400 Hz.
    static void SampleWavesForSim(void* user, const float* xy, int count, float t,
                                  float* outElevation, float* outSlope);
    // Debug UI. One window per concern, toggled from the menu bar.
    void DrawMenuBar();
    void DrawStatsWindow();
    void DrawSailboatWindow();
    void DrawEntitiesWindow();
    void DrawWaterWindow();
    void DrawAtmosphereWindow();

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

    float m_fps = 0.0f;
    float m_fpsAccumulator = 0.0f;
    int m_fpsFrameCount = 0;
    float m_elapsedTime = 0.0f;
    bool m_vsync = false;

    // Which debug windows are open. Sail and water start open because they are the two you steer
    // and tune with; the rest are opened from the menu when wanted. Frame rate lives on the menu bar
    // itself, so closing Stats does not hide it.
    bool m_showSailboatWindow   = true;
    bool m_showWaterWindow      = true;
    bool m_showStatsWindow      = false;
    bool m_showTimingsWindow    = false;
    bool m_showEntitiesWindow   = false;
    bool m_showAtmosphereWindow = false;
    bool m_timePaused = false;     // freezes the ocean-wave clock (camera stays live)

    float m_sunElevation = 20.0f;  // degrees above horizon
    float m_sunAzimuth   = 45.0f;  // degrees

    // ---- The sailboat --------------------------------------------------------------------------
    SimBridge m_sim;
    int  m_boatEntity = -1;        // index into m_scene entities; -1 until the hull loads
    bool m_followBoat = true;      // chase camera + sailing keys, versus the free fly camera

    // Latest pose, already in the renderer's Y-up frame (SimBridge does the basis change).
    float m_boatPos[3]  = {0.0f, 0.0f, 0.0f};
    float m_boatQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    std::shared_ptr<Material> m_sailMat;
    std::shared_ptr<Material> m_sparMat;
    std::shared_ptr<Material> m_lineMat;
    std::shared_ptr<Material> m_foilMat;
    std::shared_ptr<Material> m_markerMat;
    std::shared_ptr<Material> m_telltaleMat;

    // Where each part starts in the rig mesh. Computed once, because the mast segment count is
    // only known after the first view update and everything downstream is offset by it.
    int m_boomBase = 0, m_lineBase = 0, m_foilBase = 0, m_markerBase = 0, m_telltaleBase = 0;

    static constexpr int kLineTubes     = 6;   // sheet aft span, 2 bridle, vang, then the forward                                               // lead: bail -> gooseneck block -> deck ratchet
    static constexpr int kBladeVerts    = 8;   // tapered thin box
    static constexpr int kSphereStacks  = 6, kSphereSlices = 8;
    static constexpr int kSphereVerts   = (kSphereStacks + 1) * (kSphereSlices + 1);
    static constexpr int kFlyVerts      = 7;   // flat arrow outline
    static constexpr int kTelltaleCount = 4;
    int m_sailPanelCount = 0;      // panels, fixed by the class cut once the view exists
    int m_mastSegments   = 0;      // mast centreline points - 1

    // The sim publishes ~7 sparse panels. Drawing them as flat quads leaves a faceted leech and a
    // crease at every panel boundary, so a coarse control lattice is fitted through the boundary
    // corners - with the physics two-arc camber baked in chordwise - and a tensor-product
    // Catmull-Rom patch over it is sampled at this fixed grid. C1-smooth in BOTH directions, and
    // the topology no longer depends on how many panels the class cut happens to have.
    static constexpr int   kSailRows     = 48;    // foot -> head
    static constexpr int   kSailCols     = 28;    // luff -> leech
    static constexpr int   kSailCtrlCols = 9;     // chordwise control columns of the lattice
    static constexpr float kCamberPos    = 0.35f; // draft position of the camber section

    // Low-pass time constant for the sail shape. The rig is solved at 500 Hz and drawn at frame
    // rate, and without this the leech buzzes with the aliasing between the two. Smoothed in BODY
    // frame so the shape lags while the boat's own rigid motion does not.
    static constexpr float kSailSmoothTau = 0.05f;

    std::vector<DirectX::XMFLOAT3> m_sailCtrl;        // body-frame lattice, rows x kSailCtrlCols
    std::vector<DirectX::XMFLOAT3> m_sailCtrlSmooth;  // its temporally smoothed twin
    bool m_sailSmoothInit = false;

    // The boom gets the same low-pass as the sail, so the spars and the lines hanging off them
    // follow the cloth instead of leading it.
    float m_boomYawS = 0.0f, m_boomPitchS = 0.0f;
    bool  m_boomSmoothInit = false;

    // Session settings. Wind and sea state are read at BeginLocal and are NOT live in this ABI:
    // the session desc is the only place they are expressed, so changing one restarts the race.
    float m_windSpeed   = 6.0f;    // m/s
    float m_windFromDeg = 0.0f;    // METEOROLOGICAL - the direction it blows FROM
    float m_seaState    = 0.0f;    // significant height (m); 0 publishes a calm, empty wave list
    int   m_seed        = 1234;

    // Player controls, in sim units. Defaults are the Laser fully eased (LaserDefaults: sheet
    // two-block 0.09 to stopper 4.0, vang 0.20 to 0.866, outhaul 0 to 0.25).
    float m_sheet       = 4.0f;
    float m_vang        = 0.866f;
    float m_outhaul     = 0.09253f;
    float m_helm        = 0.0f;        // rad, port positive
    float m_centreboard = 0.0f;        // retraction: 0 fully down, 1 fully up
    float m_traveler    = 1.5707963f;  // ANGLE on the traveler arc, centred at pi/2
    float m_hike        = 0.0f;
    float m_foreAft     = 0.0f;

    // ---- The water the sim samples ---------------------------------------------------------
    // This renderer owns the surface: the sim is handed a callback instead of building its own
    // seeded spectrum, so the hull floats on the ocean actually being drawn. The cache is refilled
    // once a frame, centred on the boat, and every query that tick reads it - a fresh evaluation
    // per query would be hundreds of cosines several thousand times a second.
    // Small and fine beats large and coarse. The hull is 4.2 m, so a wide footprint mostly buys
    // water nothing samples, while the mode band is what decides whether the surface the boat
    // floats on is the surface being drawn: the GPU sums 256x256 modes per cascade and a
    // truncated CPU sum does not converge - measured, it still moves by centimetres at 64 bins.
    // Cost runs as modes^2 x cells x cascades, so the metres given up here are what pays for the
    // extra bands. Anything outside the footprint clamps to its edge.
    static constexpr int   kWaveCacheN     = 48;     // samples per side
    static constexpr float kWaveCacheSide  = 8.0f;   // metres covered, centred on the boat
    static constexpr int   kWaveCacheModes = 16;     // spectrum bins per axis per cascade

    std::vector<float> m_waveCache;        // kWaveCacheN^2 heights, row-major
    float m_waveCacheOriginX = 0.0f;       // world XZ of cell (0,0)
    float m_waveCacheOriginZ = 0.0f;
    bool  m_waveCacheValid   = false;

    // One clock for the water: the sim's phase time, which freezes on a pause while the render
    // clock keeps running. The cache is filled at it AND the surface is drawn at it, so what the
    // hull sits on and what the player sees are the same instant rather than a frame apart.
    float m_waterTime = 0.0f;

    // What refilling the cache costs, in milliseconds. On screen because the mode band is a
    // direct trade against frame time and the honest way to pick it is to watch both.
    float m_waveCacheMs = 0.0f;

    // True once the cache is being filled from the GPU readback rather than the CPU series.
    bool m_waveCacheFromGpu = false;

    // Keep the wave spectrum pointed down the sim's mean breeze. On by default: a sea running across
    // the wind is the kind of wrong that is hard to name but obvious once seen.
    bool m_waterFollowsSimWind = true;

    // Spring-arm chase camera state.
    float m_camOrbitYaw   = 0.6f;   // quarter view: dead astern looks down the sail edge-on
    float m_camOrbitPitch = 0.0f;
    float m_camZoom       = 1.0f;
    bool  m_cameraInitialised = false;
    DirectX::XMFLOAT3 m_smoothedCamPos = {0.0f, 0.0f, 0.0f};


};
