#include "FlyCamera.h"
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace DirectX;

void FlyCamera::SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ) {
    m_fovY = fovY;
    m_aspectRatio = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;
}

void FlyCamera::SetPosition(const XMFLOAT3& pos) {
    m_position = pos;
}

void FlyCamera::SetMoveSpeed(float speed) {
    m_moveSpeed = speed;
}

void FlyCamera::SetLookSensitivity(float sensitivity) {
    m_sensitivity = sensitivity;
}

void FlyCamera::OnMouseButtonDown(int button, int x, int y) {
    if (button == 1) {
        m_looking = true;
        m_lastMouseX = x;
        m_lastMouseY = y;
    }
}

void FlyCamera::OnMouseButtonUp(int button) {
    if (button == 1) {
        m_looking = false;
        m_forward = m_back = m_left = m_right = m_up = m_down = m_boost = false;
    }
}

void FlyCamera::OnMouseMove(int x, int y) {
    if (!m_looking) return;

    int dx = x - m_lastMouseX;
    int dy = y - m_lastMouseY;
    m_lastMouseX = x;
    m_lastMouseY = y;

    m_yaw += dx * m_sensitivity;
    m_pitch += dy * m_sensitivity;

    const float limit = XM_PIDIV2 - 0.01f;
    if (m_pitch > limit) m_pitch = limit;
    if (m_pitch < -limit) m_pitch = -limit;
}

void FlyCamera::OnMouseWheel(int delta) {
    m_moveSpeed += delta * 0.5f / WHEEL_DELTA;
    if (m_moveSpeed < 0.1f) m_moveSpeed = 0.1f;
    if (m_moveSpeed > 50.0f) m_moveSpeed = 50.0f;
}

void FlyCamera::OnKeyDown(int key) {
    if (!m_looking) return;
    switch (key) {
        case 'W': m_forward = true; break;
        case 'S': m_back = true; break;
        case 'A': m_left = true; break;
        case 'D': m_right = true; break;
        case 'E': m_up = true; break;
        case 'Q': m_down = true; break;
        case VK_SHIFT: m_boost = true; break;
    }
}

void FlyCamera::OnKeyUp(int key) {
    switch (key) {
        case 'W': m_forward = false; break;
        case 'S': m_back = false; break;
        case 'A': m_left = false; break;
        case 'D': m_right = false; break;
        case 'E': m_up = false; break;
        case 'Q': m_down = false; break;
        case VK_SHIFT: m_boost = false; break;
    }
}

void FlyCamera::Update(float dt) {
    // Forward vector from yaw/pitch
    XMVECTOR forward = XMVectorSet(
        cosf(m_pitch) * sinf(m_yaw),
        -sinf(m_pitch),
        cosf(m_pitch) * cosf(m_yaw), 0.0f);
    XMVECTOR right = XMVectorSet(cosf(m_yaw), 0.0f, -sinf(m_yaw), 0.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR moveDir = XMVectorZero();
    if (m_forward) moveDir = XMVectorAdd(moveDir, forward);
    if (m_back)    moveDir = XMVectorSubtract(moveDir, forward);
    if (m_right)   moveDir = XMVectorAdd(moveDir, right);
    if (m_left)    moveDir = XMVectorSubtract(moveDir, right);
    if (m_up)      moveDir = XMVectorAdd(moveDir, up);
    if (m_down)    moveDir = XMVectorSubtract(moveDir, up);

    // Normalize if moving
    if (XMVectorGetX(XMVector3LengthSq(moveDir)) > 0.0f) {
        moveDir = XMVector3Normalize(moveDir);
    }

    float speed = m_moveSpeed * (m_boost ? m_boostMultiplier : 1.0f);
    XMVECTOR pos = XMLoadFloat3(&m_position);
    pos = XMVectorAdd(pos, XMVectorScale(moveDir, speed * dt));
    XMStoreFloat3(&m_position, pos);
}

XMMATRIX FlyCamera::GetViewMatrix() const {
    XMVECTOR eye = XMLoadFloat3(&m_position);
    XMVECTOR forward = XMVectorSet(
        cosf(m_pitch) * sinf(m_yaw),
        -sinf(m_pitch),
        cosf(m_pitch) * cosf(m_yaw), 0.0f);
    XMVECTOR target = XMVectorAdd(eye, forward);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    return XMMatrixLookAtLH(eye, target, up);
}

XMMATRIX FlyCamera::GetProjectionMatrix() const {
    return XMMatrixPerspectiveFovLH(m_fovY, m_aspectRatio, m_nearZ, m_farZ);
}
