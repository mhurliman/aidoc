#pragma once

#include <DirectXMath.h>

class InputManager;

class FlyCamera
{
public:
    void SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ);
    void SetPosition(const DirectX::XMFLOAT3& pos);
    void SetMoveSpeed(float speed);
    void SetLookSensitivity(float sensitivity);

    // Place the camera <distance> behind and <height> above <target>, looking at it.
    // Used by the boat-follow mode so the sim boat is always framed regardless of drift.
    void FrameTarget(const DirectX::XMFLOAT3& target, float distance, float height);

    // Position the camera at <eye> looking at <target> (both world space). Used by the chase cam,
    // which computes its own smoothed eye/target from the boat pose.
    void SetLookAt(const DirectX::XMFLOAT3& eye, const DirectX::XMFLOAT3& target);

    void Update(float dt, const InputManager& input);

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMFLOAT3 GetPosition() const { return m_position; }
    float GetMoveSpeed() const { return m_moveSpeed; }
    float& GetMoveSpeed() { return m_moveSpeed; }

private:
    DirectX::XMFLOAT3 m_position = {0.0f, 0.0f, -3.0f};
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;

    float m_fovY = DirectX::XM_PIDIV4;
    float m_aspectRatio = 16.0f / 9.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;

    float m_moveSpeed = 10.0f;
    float m_boostMultiplier = 3.0f;
    float m_sensitivity = 0.003f;
    float m_keyLookSpeed = 1.5f;
};
