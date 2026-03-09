#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "Mesh.h"
#include "Material.h"
#include "Texture.h"
#include "../managers/ResourceManager.h"
#include "../helpers/Assert.h"
#include <cassert>
#include <cstring>
#include <functional>

using namespace DirectX;

static const float SCENE_SCALE = 0.01f;

void Mesh::LoadFromGltf(const std::string& path, ID3D12Device* device,
                        ID3D12GraphicsCommandList* cmdList, ResourceManager& resMgr)
{
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool loaded = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    assert(loaded && "Failed to load glTF file");

    // Load images as Texture resources
    std::vector<std::shared_ptr<Texture>> textures;
    for (size_t i = 0; i < model.images.size(); i++)
    {
        const auto& image = model.images[i];
        std::string key = path + "#image" + std::to_string(i);

        // Convert to RGBA if needed
        std::vector<uint8_t> rgbaPixels;
        const uint8_t* pixels = nullptr;

        if (image.component == 4)
        {
            pixels = image.image.data();
        }
        else if (image.component == 3)
        {
            rgbaPixels.resize(image.width * image.height * 4);
            for (int p = 0; p < image.width * image.height; p++)
            {
                rgbaPixels[p * 4 + 0] = image.image[p * 3 + 0];
                rgbaPixels[p * 4 + 1] = image.image[p * 3 + 1];
                rgbaPixels[p * 4 + 2] = image.image[p * 3 + 2];
                rgbaPixels[p * 4 + 3] = 255;
            }
            pixels = rgbaPixels.data();
        }

        auto tex = resMgr.CreateTexture(key, pixels, image.width, image.height, device, cmdList);
        textures.push_back(tex);
    }

    // Parse materials
    for (size_t i = 0; i < model.materials.size(); i++)
    {
        const auto& gltfMat = model.materials[i];
        std::string key = path + "#material" + std::to_string(i);

        auto mat = resMgr.CreateMaterial(key);
        mat->name = gltfMat.name;
        const auto& pbr = gltfMat.pbrMetallicRoughness;
        mat->roughness = static_cast<float>(pbr.roughnessFactor);
        mat->metallic = static_cast<float>(pbr.metallicFactor);
        mat->baseColorFactor = {
            static_cast<float>(pbr.baseColorFactor[0]), static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]), static_cast<float>(pbr.baseColorFactor[3])};

        if (pbr.baseColorTexture.index >= 0)
        {
            int texIdx = pbr.baseColorTexture.index;
            int imgIdx = model.textures[texIdx].source;
            if (imgIdx >= 0 && imgIdx < static_cast<int>(textures.size()))
            {
                mat->baseColorTexture = textures[imgIdx];
            }
        }

        if (gltfMat.emissiveTexture.index >= 0)
        {
            int texIdx = gltfMat.emissiveTexture.index;
            int imgIdx = model.textures[texIdx].source;
            if (imgIdx >= 0 && imgIdx < static_cast<int>(textures.size()))
            {
                mat->emissiveTexture = textures[imgIdx];
            }
        }

        mat->emissiveFactor = {static_cast<float>(gltfMat.emissiveFactor[0]),
                               static_cast<float>(gltfMat.emissiveFactor[1]),
                               static_cast<float>(gltfMat.emissiveFactor[2])};
        mat->doubleSided = gltfMat.doubleSided;
        mat->alphaBlend = (gltfMat.alphaMode == "BLEND");

        m_materials.push_back(mat);
    }

    // Walk node hierarchy to extract geometry with transforms baked in
    std::function<void(int, XMMATRIX)> walkNode = [&](int nodeIdx, XMMATRIX parentTransform)
    {
        const auto& node = model.nodes[nodeIdx];

        XMMATRIX localTransform = XMMatrixIdentity();
        if (node.matrix.size() == 16)
        {
            XMFLOAT4X4 mat;
            float* dst = reinterpret_cast<float*>(&mat);
            for (int i = 0; i < 16; i++)
            {
                dst[i] = static_cast<float>(node.matrix[i]);
            }
            localTransform = XMLoadFloat4x4(&mat);
        }

        XMMATRIX worldTransform = localTransform * parentTransform;

        if (node.mesh >= 0)
        {
            const auto& mesh = model.meshes[node.mesh];
            for (const auto& primitive : mesh.primitives)
            {
                if (primitive.mode != 4 && primitive.mode != -1)
                {
                    continue;
                }

                SubMesh subMesh;
                subMesh.name = mesh.name;
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

                    if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                    {
                        memcpy(&m_indices[offset], data, acc.count * sizeof(uint32_t));
                    }
                    else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    {
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
                        for (size_t i = 0; i < acc.count; i++)
                        {
                            m_indices[offset + i] = src[i];
                        }
                    }
                }

                // Helper to get attribute data pointer and stride
                auto getAttrData = [&](const char* name,
                                       int expectedComponents) -> std::pair<const uint8_t*, size_t>
                {
                    auto it = primitive.attributes.find(name);
                    if (it == primitive.attributes.end())
                    {
                        return {nullptr, 0};
                    }
                    const auto& acc = model.accessors[it->second];
                    const auto& view = model.bufferViews[acc.bufferView];
                    const auto& buf = model.buffers[view.buffer];
                    const uint8_t* ptr = buf.data.data() + view.byteOffset + acc.byteOffset;
                    size_t stride = view.byteStride;
                    if (stride == 0)
                    {
                        stride = tinygltf::GetComponentSizeInBytes(
                                     static_cast<uint32_t>(acc.componentType)) *
                                 expectedComponents;
                    }
                    return {ptr, stride};
                };

                auto [posData, posStride] = getAttrData("POSITION", 3);
                if (!posData)
                {
                    continue;
                }

                const auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
                size_t vertexCount = posAcc.count;

                auto [normData, normStride] = getAttrData("NORMAL", 3);
                auto [uvData, uvStride] = getAttrData("TEXCOORD_0", 2);

                XMMATRIX scaledWorld =
                    worldTransform * XMMatrixScaling(SCENE_SCALE, SCENE_SCALE, SCENE_SCALE);

                size_t vertOffset = m_vertices.size();
                m_vertices.resize(vertOffset + vertexCount);

                for (size_t i = 0; i < vertexCount; i++)
                {
                    auto& vert = m_vertices[vertOffset + i];

                    const float* p = reinterpret_cast<const float*>(posData + i * posStride);
                    XMVECTOR pos = XMVectorSet(p[0], p[1], p[2], 1.0f);
                    pos = XMVector3TransformCoord(pos, scaledWorld);
                    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(vert.position), pos);

                    if (normData)
                    {
                        const float* n = reinterpret_cast<const float*>(normData + i * normStride);
                        XMVECTOR norm = XMVectorSet(n[0], n[1], n[2], 0.0f);
                        norm = XMVector3Normalize(XMVector3TransformNormal(norm, worldTransform));
                        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(vert.normal), norm);
                    }
                    else
                    {
                        vert.normal[0] = 0.0f;
                        vert.normal[1] = 1.0f;
                        vert.normal[2] = 0.0f;
                    }

                    if (uvData)
                    {
                        const float* uv = reinterpret_cast<const float*>(uvData + i * uvStride);
                        vert.uv[0] = uv[0];
                        vert.uv[1] = uv[1];
                    }
                    else
                    {
                        vert.uv[0] = 0.0f;
                        vert.uv[1] = 0.0f;
                    }
                }

                m_subMeshes.push_back(subMesh);
            }
        }

        for (int child : node.children)
        {
            walkNode(child, worldTransform);
        }
    };

    if (!model.scenes.empty())
    {
        int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
        const auto& scene = model.scenes[sceneIdx];
        for (int root : scene.nodes)
        {
            walkNode(root, XMMatrixIdentity());
        }
    }

    // Create GPU buffers for geometry
    CreateBuffers(device);
}

void Mesh::CreateBuffers(ID3D12Device* device)
{
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

        ASSERT_SUCCEEDED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_vertexBuffer)));

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        ASSERT_SUCCEEDED(m_vertexBuffer->Map(0, &readRange, &mapped));
        memcpy(mapped, m_vertices.data(), size);
        m_vertexBuffer->Unmap(0, nullptr);

        m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vbv.StrideInBytes = sizeof(PbrVertex);
        m_vbv.SizeInBytes = size;
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

        ASSERT_SUCCEEDED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&m_indexBuffer)));

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        ASSERT_SUCCEEDED(m_indexBuffer->Map(0, &readRange, &mapped));
        memcpy(mapped, m_indices.data(), size);
        m_indexBuffer->Unmap(0, nullptr);

        m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_ibv.Format = DXGI_FORMAT_R32_UINT;
        m_ibv.SizeInBytes = size;
    }
}
