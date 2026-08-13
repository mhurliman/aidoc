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
    void LoadProps();          // load floating OBJ props onto the ocean
    void UpdateProps();        // per-frame single-point buoyancy for the props
    void SetupWaveDebug();     // build the CPU-vs-GPU surface overlay (materials + tile mesh)
    void UpdateDebugWaveMesh();// refill the overlay each frame
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
    bool m_timePaused = false;     // freezes the ocean-wave clock (camera stays live)

    float m_sunElevation = 20.0f;  // degrees above horizon
    float m_sunAzimuth   = 45.0f;  // degrees

    // Floating OBJ props (single-point buoyancy on the FFT ocean), scattered around the origin.
    struct FloatingProp
    {
        int   entity   = -1;            // index into m_scene entities
        float x = 0.0f, z = 0.0f;       // world XZ where it bobs
        float yaw      = 0.0f;          // random heading (radians)
        float scale    = 1.0f;          // normalizes each model to a common world size
        float center[3] = {0, 0, 0};    // model-space AABB center (pivot-independent float)
    };
    std::vector<FloatingProp> m_props;
    float m_propX    = 0.0f;   // wave-debug grid center (origin)
    float m_propZ    = 0.0f;
    float m_propSink = 0.0f;   // global waterline offset applied to all props

    // Wave debug overlay: green tiles = CPU buoyancy surface (SampleHeightCPU), magenta tiles =
    // GPU/render surface (high-mode height + choppiness displacement). The gap is the disparity.
    bool m_showWaveDebug = false;
    std::shared_ptr<Material> m_waveDebugMat;  // magenta (GPU surface)
    std::shared_ptr<Material> m_physSurfMat;   // green (buoyancy surface)
    std::vector<float> m_dbgH, m_dbgDx, m_dbgDz;
};
