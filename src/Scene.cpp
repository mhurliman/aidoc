#include <tiny_gltf.h>
#include "Scene.h"
#include <stdexcept>
#include <cstring>
#include <functional>

using namespace DirectX;

static const float SCENE_SCALE = 0.01f;

static inline void ThrowIfFailed(HRESULT hr, const char* msg) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(msg) + " (hr=0x" +
            std::to_string(hr) + ")");
    }
}

void Scene::LoadFromGltf(const std::string& gltfPath) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    if (!loader.LoadASCIIFromFile(&model, &err, &warn, gltfPath)) {
        throw std::runtime_error("Failed to load glTF: " + err);
    }

    // Parse materials
    for (const auto& mat : model.materials) {
        PbrMaterial pbrMat;
        const auto& pbr = mat.pbrMetallicRoughness;
        pbrMat.roughness = static_cast<float>(pbr.roughnessFactor);
        pbrMat.metallic = static_cast<float>(pbr.metallicFactor);
        pbrMat.baseColorFactor = {
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        };
        if (pbr.baseColorTexture.index >= 0) {
            int texIdx = pbr.baseColorTexture.index;
            pbrMat.baseColorTextureIndex = model.textures[texIdx].source;
        }
        if (mat.emissiveTexture.index >= 0) {
            int texIdx = mat.emissiveTexture.index;
            pbrMat.emissiveTextureIndex = model.textures[texIdx].source;
        }
        pbrMat.emissiveFactor = {
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        };
        pbrMat.doubleSided = mat.doubleSided;
        pbrMat.alphaBlend = (mat.alphaMode == "BLEND");
        m_materials.push_back(pbrMat);
    }

    // Load images as RGBA
    for (const auto& image : model.images) {
        CpuTexture tex;
        tex.width = image.width;
        tex.height = image.height;
        if (image.component == 4) {
            tex.pixels = image.image;
        } else if (image.component == 3) {
            tex.pixels.resize(image.width * image.height * 4);
            for (int i = 0; i < image.width * image.height; i++) {
                tex.pixels[i * 4 + 0] = image.image[i * 3 + 0];
                tex.pixels[i * 4 + 1] = image.image[i * 3 + 1];
                tex.pixels[i * 4 + 2] = image.image[i * 3 + 2];
                tex.pixels[i * 4 + 3] = 255;
            }
        }
        m_cpuTextures.push_back(std::move(tex));
    }

    // Walk node hierarchy to extract geometry with transforms baked in
    std::function<void(int, XMMATRIX)> walkNode = [&](int nodeIdx, XMMATRIX parentTransform) {
        const auto& node = model.nodes[nodeIdx];

        XMMATRIX localTransform = XMMatrixIdentity();
        if (node.matrix.size() == 16) {
            XMFLOAT4X4 mat;
            float* dst = reinterpret_cast<float*>(&mat);
            for (int i = 0; i < 16; i++)
                dst[i] = static_cast<float>(node.matrix[i]);
            localTransform = XMLoadFloat4x4(&mat);
        }

        XMMATRIX worldTransform = localTransform * parentTransform;

        if (node.mesh >= 0) {
            const auto& mesh = model.meshes[node.mesh];
            for (const auto& primitive : mesh.primitives) {
                if (primitive.mode != 4 && primitive.mode != -1) continue;

                SubMesh subMesh;
                subMesh.materialIndex = primitive.material;
                subMesh.startIndex = static_cast<UINT>(m_indices.size());
                subMesh.baseVertex = static_cast<INT>(m_vertices.size());

                // Read indices
                {
                    const auto& acc = model.accessors[primitive.indices];
                    const auto& view = model.bufferViews[acc.bufferView];
                    const auto& buf = model.buffers[view.buffer];
                    const uint8_t* data = buf.data.data() + view.byteOffset + acc.byteOffset;

                    subMesh.indexCount = static_cast<UINT>(acc.count);
                    size_t offset = m_indices.size();
                    m_indices.resize(offset + acc.count);

                    if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        memcpy(&m_indices[offset], data, acc.count * sizeof(uint32_t));
                    } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
                        for (size_t i = 0; i < acc.count; i++)
                            m_indices[offset + i] = src[i];
                    }
                }

                // Helper to get attribute data pointer and stride
                auto getAttrData = [&](const char* name, int expectedComponents)
                    -> std::pair<const uint8_t*, size_t> {
                    auto it = primitive.attributes.find(name);
                    if (it == primitive.attributes.end()) return { nullptr, 0 };
                    const auto& acc = model.accessors[it->second];
                    const auto& view = model.bufferViews[acc.bufferView];
                    const auto& buf = model.buffers[view.buffer];
                    const uint8_t* ptr = buf.data.data() + view.byteOffset + acc.byteOffset;
                    size_t stride = view.byteStride;
                    if (stride == 0) {
                        stride = tinygltf::GetComponentSizeInBytes(
                            static_cast<uint32_t>(acc.componentType)) * expectedComponents;
                    }
                    return { ptr, stride };
                };

                auto [posData, posStride] = getAttrData("POSITION", 3);
                if (!posData) continue;

                const auto& posAcc = model.accessors[
                    primitive.attributes.at("POSITION")];
                size_t vertexCount = posAcc.count;

                auto [normData, normStride] = getAttrData("NORMAL", 3);
                auto [uvData, uvStride] = getAttrData("TEXCOORD_0", 2);

                XMMATRIX scaledWorld = worldTransform *
                    XMMatrixScaling(SCENE_SCALE, SCENE_SCALE, SCENE_SCALE);

                size_t vertOffset = m_vertices.size();
                m_vertices.resize(vertOffset + vertexCount);

                for (size_t i = 0; i < vertexCount; i++) {
                    auto& vert = m_vertices[vertOffset + i];

                    const float* p = reinterpret_cast<const float*>(posData + i * posStride);
                    XMVECTOR pos = XMVectorSet(p[0], p[1], p[2], 1.0f);
                    pos = XMVector3TransformCoord(pos, scaledWorld);
                    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(vert.position), pos);

                    if (normData) {
                        const float* n = reinterpret_cast<const float*>(normData + i * normStride);
                        XMVECTOR norm = XMVectorSet(n[0], n[1], n[2], 0.0f);
                        norm = XMVector3Normalize(XMVector3TransformNormal(norm, worldTransform));
                        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(vert.normal), norm);
                    } else {
                        vert.normal[0] = 0.0f;
                        vert.normal[1] = 1.0f;
                        vert.normal[2] = 0.0f;
                    }

                    if (uvData) {
                        const float* uv = reinterpret_cast<const float*>(uvData + i * uvStride);
                        vert.uv[0] = uv[0];
                        vert.uv[1] = uv[1];
                    } else {
                        vert.uv[0] = 0.0f;
                        vert.uv[1] = 0.0f;
                    }
                }

                m_subMeshes.push_back(subMesh);
            }
        }

        for (int child : node.children)
            walkNode(child, worldTransform);
    };

    if (!model.scenes.empty()) {
        int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
        const auto& scene = model.scenes[sceneIdx];
        for (int root : scene.nodes)
            walkNode(root, XMMatrixIdentity());
    }
}

void Scene::CreateGpuResources(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList) {
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    // Vertex buffer
    {
        UINT size = static_cast<UINT>(m_vertices.size() * sizeof(PbrVertex));
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_vertexBuffer)),
            "Failed to create vertex buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped),
            "Failed to map vertex buffer");
        memcpy(mapped, m_vertices.data(), size);
        m_vertexBuffer->Unmap(0, nullptr);

        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.StrideInBytes = sizeof(PbrVertex);
        m_vertexBufferView.SizeInBytes = size;
    }

    // Index buffer
    {
        UINT size = static_cast<UINT>(m_indices.size() * sizeof(uint32_t));
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_indexBuffer)),
            "Failed to create index buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_indexBuffer->Map(0, &readRange, &mapped),
            "Failed to map index buffer");
        memcpy(mapped, m_indices.data(), size);
        m_indexBuffer->Unmap(0, nullptr);

        m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        m_indexBufferView.SizeInBytes = size;
    }

    // Textures - upload via command list
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (size_t i = 0; i < m_cpuTextures.size(); i++) {
        const auto& tex = m_cpuTextures[i];

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = tex.width;
        texDesc.Height = tex.height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> gpuTex;
        ThrowIfFailed(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&gpuTex)),
            "Failed to create texture");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 uploadSize;
        device->GetCopyableFootprints(&texDesc, 0, 1, 0,
            &footprint, nullptr, nullptr, &uploadSize);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> uploadBuf;
        ThrowIfFailed(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadBuf)),
            "Failed to create texture upload buffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(uploadBuf->Map(0, &readRange, &mapped),
            "Failed to map texture upload buffer");

        uint8_t* dst = static_cast<uint8_t*>(mapped) + footprint.Offset;
        UINT srcRowPitch = tex.width * 4;
        for (int row = 0; row < tex.height; row++) {
            memcpy(dst + row * footprint.Footprint.RowPitch,
                   tex.pixels.data() + row * srcRowPitch, srcRowPitch);
        }
        uploadBuf->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = gpuTex.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuf.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpuTex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        m_gpuTextures.push_back(gpuTex);
        m_uploadBuffers.push_back(uploadBuf);
    }
}

void Scene::CreateTextureSRV(ID3D12Device* device,
    D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT textureIndex) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_gpuTextures[textureIndex].Get(),
        &srvDesc, handle);
}
