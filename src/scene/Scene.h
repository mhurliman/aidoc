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

class Mesh;
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
};
