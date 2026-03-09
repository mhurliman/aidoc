#pragma once

#include <DirectXMath.h>

class InputManager;

class OrbitCamera
{
public:
    void SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ);
    void SetDistance(float distance);
    void SetOrbitSensitivity(float sensitivity);
    void SetZoomSpeed(float speed);
    void SetDistanceLimits(float minDist, float maxDist);

    void Update(float dt, const InputManager& input);

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;

private:
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_distance = 10.0f;
    DirectX::XMFLOAT3 m_target = {0.0f, 0.0f, 0.0f};

    float m_fovY = DirectX::XM_PIDIV4;
    float m_aspectRatio = 16.0f / 9.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;

    float m_sensitivity = 0.005f;
    float m_panSpeed = 0.003f;
    float m_zoomSpeed = 0.5f;
    float m_minDistance = 0.5f;
    float m_maxDistance = 50.0f;
};
