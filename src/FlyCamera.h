#pragma once

#include <DirectXMath.h>

class FlyCamera {
public:
    void SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ);
    void SetPosition(const DirectX::XMFLOAT3& pos);
    void SetMoveSpeed(float speed);
    void SetLookSensitivity(float sensitivity);

    void OnMouseButtonDown(int button, int x, int y);
    void OnMouseButtonUp(int button);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta);
    void OnKeyDown(int key);
    void OnKeyUp(int key);

    void Update(float dt);

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMFLOAT3 GetPosition() const { return m_position; }
    float GetMoveSpeed() const { return m_moveSpeed; }
    float& GetMoveSpeed() { return m_moveSpeed; }

private:
    DirectX::XMFLOAT3 m_position = { 0.0f, 0.0f, -3.0f };
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;

    float m_fovY = DirectX::XM_PIDIV4;
    float m_aspectRatio = 16.0f / 9.0f;
    float m_nearZ = 0.1f;
    float m_farZ = 100.0f;

    float m_moveSpeed = 10.0f;
    float m_boostMultiplier = 3.0f;
    float m_sensitivity = 0.003f;

    bool m_looking = false;
    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;

    bool m_forward = false;
    bool m_back = false;
    bool m_left = false;
    bool m_right = false;
    bool m_up = false;
    bool m_down = false;
    bool m_boost = false;
};
