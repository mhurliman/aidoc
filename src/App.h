#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>
#include "helpers/FlyCamera.h"
#include "helpers/InputManager.h"
#include "resources/ColorBuffer.h"
#include "resources/DepthBuffer.h"
#include "resources/Scene.h"

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
    void WaitForGpu();
    void WaitForFrame(UINT frameIndex);

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
    bool m_vsync = false;
};
