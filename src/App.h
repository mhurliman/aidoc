#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include "FlyCamera.h"
#include "ResourceManager.h"
#include "DescriptorHeap.h"

using Microsoft::WRL::ComPtr;

class Mesh;

class App {
public:
    static const UINT FrameCount = 3;
    static const UINT WindowWidth = 1280;
    static const UINT WindowHeight = 720;

    void Init(HWND hwnd);
    void Update(float dt);
    void Render();
    void Cleanup();

    FlyCamera& GetCamera() { return m_camera; }

    struct SceneConstants {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT3 cameraPos;
        float roughness;
        DirectX::XMFLOAT3 lightDir;
        float metallic;
        DirectX::XMFLOAT4 baseColorFactor;
        int hasTexture;
        DirectX::XMFLOAT3 emissiveFactor;
        int hasEmissiveTexture;
        float _pad[3];
    };

private:
    void InitDevice(HWND hwnd);
    void InitSwapChain(HWND hwnd);
    void InitRenderTargets();
    void InitDepthBuffer();
    void InitCommandList();
    void InitFence();
    void InitViewport();
    void CreatePipelineState();
    void LoadScene();
    void InitImGui(HWND hwnd);
    void WaitForGpu();
    static std::vector<BYTE> LoadShaderBlob(const std::string& filename);

    ComPtr<ID3D12Device>                m_device;
    ComPtr<ID3D12CommandQueue>          m_commandQueue;
    ComPtr<IDXGISwapChain3>             m_swapChain;
    ComPtr<ID3D12DescriptorHeap>        m_rtvHeap;
    UINT                                m_rtvDescriptorSize = 0;
    ComPtr<ID3D12Resource>              m_renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator>      m_commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList>   m_commandList;
    ComPtr<ID3D12RootSignature>         m_rootSignature;
    ComPtr<ID3D12PipelineState>         m_pipelineState;
    ComPtr<ID3D12DescriptorHeap>        m_dsvHeap;
    ComPtr<ID3D12Resource>              m_depthBuffer;
    ComPtr<ID3D12Fence>                 m_fence;
    UINT64                              m_nextFenceValue = 1;
    UINT64                              m_frameFenceValues[FrameCount] = {};
    HANDLE                              m_fenceEvent = nullptr;
    UINT                                m_frameIndex = 0;
    D3D12_VIEWPORT                      m_viewport = {};
    D3D12_RECT                          m_scissorRect = {};

    void WaitForFrame(UINT frameIndex);

    FlyCamera m_camera;
    float m_fps = 0.0f;
    float m_fpsAccumulator = 0.0f;
    int m_fpsFrameCount = 0;
    ResourceManager m_resourceManager;
    TransientDescriptorHeap m_transientHeap;
    std::shared_ptr<Mesh> m_mesh;
    bool m_vsync = false;
};
