#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../resources/IResource.h"
#include "../helpers/DescriptorHeap.h"

using Microsoft::WRL::ComPtr;

class Texture;
class Material;
class Mesh;

class ResourceManager
{
public:
    void Init(ID3D12Device* device, ID3D12CommandQueue* cmdQueue);
    void Shutdown();

    // Load a top-level resource (e.g. a Mesh from a glTF file).
    template <typename T> std::shared_ptr<T> Load(const std::string& path);

    // Create child resources (called during Mesh::LoadFromGltf).
    std::shared_ptr<Texture> CreateTexture(const std::string& key, const uint8_t* pixels, int width,
                                           int height, ID3D12Device* device,
                                           ID3D12GraphicsCommandList* cmdList);

    std::shared_ptr<Material> CreateMaterial(const std::string& key);

    // GPU lifetime management — call from App each frame.
    void DeferRelease(std::shared_ptr<IResource> resource, UINT64 fenceValue);
    void ProcessDeferredDeletions(UINT64 completedFenceValue);

    PersistentDescriptorHeap& GetSRVHeap() { return m_srvHeap; }
    ID3D12Device* GetDevice() const { return m_device; }

private:
    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_cmdQueue = nullptr;

    // Dedicated command infrastructure for synchronous resource uploads
    ComPtr<ID3D12CommandAllocator> m_uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_uploadCmdList;
    ComPtr<ID3D12Fence> m_uploadFence;
    UINT64 m_uploadFenceValue = 0;
    HANDLE m_uploadFenceEvent = nullptr;

    PersistentDescriptorHeap m_srvHeap;

    // Resource cache keyed by path/key
    std::unordered_map<std::string, std::shared_ptr<IResource>> m_cache;

    // Deferred deletion queue
    struct PendingDeletion
    {
        UINT64 fenceValue;
        std::shared_ptr<IResource> resource;
    };
    std::vector<PendingDeletion> m_deletionQueue;

    void ExecuteUploadAndWait();
};
