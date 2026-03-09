#include "Scene.h"
#include "Mesh.h"
#include "Material.h"
#include "../managers/ResourceManager.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <fstream>

using namespace DirectX;

void Scene::Load(const std::string& jsonPath, ID3D12Device* device, ResourceManager& resMgr)
{
    std::ifstream file(jsonPath);
    assert(file.is_open() && "Failed to open scene file");

    nlohmann::json root = nlohmann::json::parse(file);

    // Resolve mesh paths relative to the executable directory (not the scene file)
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeDir(exePath);
    auto pos = exeDir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        exeDir = exeDir.substr(0, pos + 1);
    }

    for (const auto& entityJson : root["entities"])
    {
        Entity entity;

        // Load mesh (ResourceManager caches by path, so duplicates share the resource)
        std::string meshPath = entityJson["mesh"].get<std::string>();
        entity.mesh = resMgr.Load<Mesh>(exeDir + meshPath);

        // Determine shading model for this entity
        ShadingModel shading = ShadingModel::PBR;
        if (entityJson.contains("shader"))
        {
            std::string shaderName = entityJson["shader"].get<std::string>();
            if (shaderName == "phong")
            {
                shading = ShadingModel::Phong;
            }
        }

        // Create PSOs for all materials in this mesh
        for (auto& mat : entity.mesh->GetMaterials())
        {
            mat->shadingModel = shading;
            if (!mat->GetPSO())
            {
                mat->CreatePSO(device);
            }
        }

        // World transform — defaults to identity if not specified
        XMStoreFloat4x4(&entity.worldTransform, XMMatrixIdentity());
        if (entityJson.contains("transform"))
        {
            const auto& t = entityJson["transform"];
            if (t.size() == 16)
            {
                float* dst = reinterpret_cast<float*>(&entity.worldTransform);
                for (int i = 0; i < 16; i++)
                {
                    dst[i] = t[i].get<float>();
                }
            }
        }

        m_entities.push_back(std::move(entity));
    }

    // Load lights
    if (root.contains("lights"))
    {
        for (const auto& lightJson : root["lights"])
        {
            std::string type = lightJson["type"].get<std::string>();

            if (type == "directional")
            {
                DirectionalLight light;
                if (lightJson.contains("color"))
                {
                    const auto& c = lightJson["color"];
                    light.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                if (lightJson.contains("direction"))
                {
                    const auto& d = lightJson["direction"];
                    light.direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                }
                m_directionalLights.push_back(light);
            }
            else if (type == "point")
            {
                PointLight light;
                if (lightJson.contains("color"))
                {
                    const auto& c = lightJson["color"];
                    light.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                if (lightJson.contains("position"))
                {
                    const auto& p = lightJson["position"];
                    light.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
                }
                if (lightJson.contains("linearFalloff"))
                {
                    light.linearFalloff = lightJson["linearFalloff"].get<float>();
                }
                if (lightJson.contains("quadraticFalloff"))
                {
                    light.quadraticFalloff = lightJson["quadraticFalloff"].get<float>();
                }
                m_pointLights.push_back(light);
            }
            else if (type == "spot")
            {
                SpotLight light;
                if (lightJson.contains("color"))
                {
                    const auto& c = lightJson["color"];
                    light.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                if (lightJson.contains("position"))
                {
                    const auto& p = lightJson["position"];
                    light.position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
                }
                if (lightJson.contains("direction"))
                {
                    const auto& d = lightJson["direction"];
                    light.direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
                }
                if (lightJson.contains("linearFalloff"))
                {
                    light.linearFalloff = lightJson["linearFalloff"].get<float>();
                }
                if (lightJson.contains("quadraticFalloff"))
                {
                    light.quadraticFalloff = lightJson["quadraticFalloff"].get<float>();
                }
                if (lightJson.contains("angle"))
                {
                    light.angle = lightJson["angle"].get<float>() * 3.14159265359f / 180.0f;
                }
                m_spotLights.push_back(light);
            }
        }
    }
}
