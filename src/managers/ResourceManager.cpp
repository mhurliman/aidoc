#include "ResourceManager.h"
#include "../resources/Texture.h"
#include "../resources/Material.h"
#include "../resources/Mesh.h"
#include "../helpers/Assert.h"
#include <cassert>

void ResourceManager::Init(ID3D12Device* device, ID3D12CommandQueue* cmdQueue)
{
    m_device = device;
    m_cmdQueue = cmdQueue;

    // Persistent CPU-only SRV heap (256 slots)
    m_srvHeap.Init(device, 256);

    // Create dedicated upload command infrastructure
    ASSERT_SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&m_uploadAllocator)));

    ASSERT_SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               m_uploadAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_uploadCmdList)));
    m_uploadCmdList->Close();

    ASSERT_SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_uploadFence)));

    m_uploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    assert(m_uploadFenceEvent && "Failed to create upload fence event");
}

void ResourceManager::Shutdown()
{
    // Drain all pending deletions
    m_deletionQueue.clear();
    m_cache.clear();

    if (m_uploadFenceEvent)
    {
        CloseHandle(m_uploadFenceEvent);
        m_uploadFenceEvent = nullptr;
    }
}

// Template specialization for Mesh
template <> std::shared_ptr<Mesh> ResourceManager::Load(const std::string& path)
{
    // Check cache
    auto it = m_cache.find(path);
    if (it != m_cache.end())
    {
        return std::static_pointer_cast<Mesh>(it->second);
    }

    // Reset upload command list for recording texture uploads
    ASSERT_SUCCEEDED(m_uploadAllocator->Reset());
    ASSERT_SUCCEEDED(m_uploadCmdList->Reset(m_uploadAllocator.Get(), nullptr));

    auto mesh = std::make_shared<Mesh>();
    mesh->LoadFromGltf(path, m_device, m_uploadCmdList.Get(), *this);

    // Execute upload commands and wait
    ExecuteUploadAndWait();

    m_cache[path] = mesh;
    return mesh;
}

std::shared_ptr<Texture> ResourceManager::CreateTexture(const std::string& key,
                                                        const uint8_t* pixels, int width,
                                                        int height, ID3D12Device* device,
                                                        ID3D12GraphicsCommandList* cmdList)
{

    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        return std::static_pointer_cast<Texture>(it->second);
    }

    auto tex = std::make_shared<Texture>();
    tex->CreateFromPixels(device, cmdList, pixels, width, height, m_srvHeap);
    m_cache[key] = tex;
    return tex;
}

std::shared_ptr<Material> ResourceManager::CreateMaterial(const std::string& key)
{
    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        return std::static_pointer_cast<Material>(it->second);
    }

    auto mat = std::make_shared<Material>();
    m_cache[key] = mat;
    return mat;
}

void ResourceManager::DeferRelease(std::shared_ptr<IResource> resource, UINT64 fenceValue)
{
    m_deletionQueue.push_back({fenceValue, std::move(resource)});
}

void ResourceManager::ProcessDeferredDeletions(UINT64 completedFenceValue)
{
    m_deletionQueue.erase(std::remove_if(m_deletionQueue.begin(), m_deletionQueue.end(),
                                         [completedFenceValue](const PendingDeletion& pd)
                                         { return pd.fenceValue <= completedFenceValue; }),
                          m_deletionQueue.end());
}

void ResourceManager::ExecuteUploadAndWait()
{
    ASSERT_SUCCEEDED(m_uploadCmdList->Close());

    ID3D12CommandList* lists[] = {m_uploadCmdList.Get()};
    m_cmdQueue->ExecuteCommandLists(1, lists);

    m_uploadFenceValue++;
    ASSERT_SUCCEEDED(m_cmdQueue->Signal(m_uploadFence.Get(), m_uploadFenceValue));

    if (m_uploadFence->GetCompletedValue() < m_uploadFenceValue)
    {
        ASSERT_SUCCEEDED(
            m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, m_uploadFenceEvent));
        WaitForSingleObject(m_uploadFenceEvent, INFINITE);
    }
}
