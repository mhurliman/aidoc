#pragma once

#include <DirectXMath.h>

class Scene;

struct View
{
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;
    DirectX::XMFLOAT4X4 viewProjMatrix;
    DirectX::XMFLOAT3 position;
};

struct FrameConstants
{
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3 cameraPos;
    int numLights;
};

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual void RenderScene(const Scene& scene, const View& view,
                             const FrameConstants& frameConstants) = 0;
};
