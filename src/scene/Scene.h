#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <memory>

#include "Light.h"
#include "water/FFTWaterSurface.h"
#include "gfx/Atmosphere.h"
#include "assets/Mesh.h"   // PbrVertex

class ResourceManager;
class GraphicsContext;
class LinearAllocator;
class TransientDescriptorHeap;

struct Entity
{
    std::shared_ptr<Mesh> mesh;
    DirectX::XMFLOAT4X4 worldTransform;
    bool visible = true;
};

// A CPU-side dynamic mesh rebuilt every frame (the sim rig: sail + spars + lines). Vertices are
// re-uploaded through the renderer's per-frame linear allocator, so no persistent GPU buffer is
// needed. Indices are fixed once. Each submesh binds its own (already-PSO'd) material.
struct DynamicMesh
{
    struct SubMesh { uint32_t startIndex; uint32_t indexCount; std::shared_ptr<Material> material; };
    std::vector<PbrVertex> vertices;   // rewritten each frame (positions + normals; uv unused)
    std::vector<uint32_t>  indices;    // set once
    std::vector<SubMesh>   submeshes;  // set once
    bool visible = false;
};

class Scene
{
public:
    // Load a scene from a JSON configuration file.
    // Mesh paths in the JSON are resolved relative to the executable directory.
    void Load(const std::string& jsonPath, ID3D12Device* device, ResourceManager& resMgr);

    void CreateWater(ID3D12Device* device, const WaterDesc& desc, ID3D12CommandQueue* queue);
    void CreateAtmosphere(ID3D12Device* device);

    FFTWaterSurface& GetWaterSurface()   { return m_water; }
    Atmosphere&      GetAtmosphere()     { return m_atmosphere; }
    const FFTWaterSurface& GetWaterSurface() const { return m_water; }
    std::vector<Entity>&       GetEntities()       { return m_entities; }
    const std::vector<Entity>& GetEntities() const { return m_entities; }

    DynamicMesh&       GetRig()       { return m_rig; }
    const DynamicMesh& GetRig() const { return m_rig; }

    DynamicMesh&       GetDebug()       { return m_debug; }
    const DynamicMesh& GetDebug() const { return m_debug; }

    std::vector<DirectionalLight>& GetDirectionalLights() { return m_directionalLights; }
    std::vector<PointLight>& GetPointLights() { return m_pointLights; }
    std::vector<SpotLight>& GetSpotLights() { return m_spotLights; }

    const std::vector<DirectionalLight>& GetDirectionalLights() const { return m_directionalLights; }
    const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }
    const std::vector<SpotLight>& GetSpotLights() const { return m_spotLights; }

private:
    std::vector<Entity> m_entities;
    std::vector<DirectionalLight> m_directionalLights;
    std::vector<PointLight> m_pointLights;
    std::vector<SpotLight> m_spotLights;
    FFTWaterSurface m_water;
    Atmosphere      m_atmosphere;
    DynamicMesh     m_rig;
    DynamicMesh     m_debug;   // optional overlay (e.g. CPU wave-sample markers)
};
