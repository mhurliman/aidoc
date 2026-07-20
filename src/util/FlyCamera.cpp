#include "FlyCamera.h"
#include "InputManager.h"
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace DirectX;

void FlyCamera::SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ)
{
    m_fovY = fovY;
    m_aspectRatio = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;
}

void FlyCamera::SetPosition(const XMFLOAT3& pos)
{
    m_position = pos;
}

void FlyCamera::SetMoveSpeed(float speed)
{
    m_moveSpeed = speed;
}

void FlyCamera::SetLookSensitivity(float sensitivity)
{
    m_sensitivity = sensitivity;
}

void FlyCamera::Update(float dt, const InputManager& input)
{
    // Mouse look: only when right button is held (returns false if consumed)
    bool looking = input.IsMouseButtonDown(1);

    if (looking)
    {
        // GetMouseDelta returns 0 if mouse move is consumed
        float dx = static_cast<float>(input.GetMouseDeltaX());
        float dy = static_cast<float>(input.GetMouseDeltaY());

        m_yaw   += dx * m_sensitivity;
        m_pitch += dy * m_sensitivity;

        const float limit = XM_PIDIV2 - 0.01f;
        if (m_pitch >  limit) m_pitch =  limit;
        if (m_pitch < -limit) m_pitch = -limit;
    }

    // Mouse wheel: adjust move speed (returns 0 if consumed)
    int wheel = input.GetMouseWheelDelta();
    if (wheel != 0)
    {
        m_moveSpeed += static_cast<float>(wheel) * 0.5f / static_cast<float>(WHEEL_DELTA);
        if (m_moveSpeed < 0.1f)
        {
            m_moveSpeed = 0.1f;
        }
        if (m_moveSpeed > 50.0f)
        {
            m_moveSpeed = 50.0f;
        }
    }

    // Arrow keys: look without requiring mouse button
    float lookRate = m_keyLookSpeed * dt;
    if (input.IsKeyDown(VK_LEFT))  m_yaw   -= lookRate;
    if (input.IsKeyDown(VK_RIGHT)) m_yaw   += lookRate;
    if (input.IsKeyDown(VK_UP))    m_pitch -= lookRate;
    if (input.IsKeyDown(VK_DOWN))  m_pitch += lookRate;

    {
        const float limit = XM_PIDIV2 - 0.01f;
        if (m_pitch >  limit) m_pitch =  limit;
        if (m_pitch < -limit) m_pitch = -limit;
    }

    // WASD movement: always active (IsKeyDown returns false if consumed)
    bool fwd   = input.IsKeyDown('W');
    bool back  = input.IsKeyDown('S');
    bool left  = input.IsKeyDown('A');
    bool right = input.IsKeyDown('D');
    bool up    = input.IsKeyDown('E');
    bool down  = input.IsKeyDown('Q');
    bool boost = input.IsKeyDown(VK_SHIFT);

    // Forward vector from yaw/pitch
    XMVECTOR forward =
        XMVectorSet(cosf(m_pitch) * sinf(m_yaw), -sinf(m_pitch), cosf(m_pitch) * cosf(m_yaw), 0.0f);
    XMVECTOR rightVec = XMVectorSet(cosf(m_yaw), 0.0f, -sinf(m_yaw), 0.0f);
    XMVECTOR upVec = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR moveDir = XMVectorZero();
    if (fwd)
    {
        moveDir = XMVectorAdd(moveDir, forward);
    }
    if (back)
    {
        moveDir = XMVectorSubtract(moveDir, forward);
    }
    if (right)
    {
        moveDir = XMVectorAdd(moveDir, rightVec);
    }
    if (left)
    {
        moveDir = XMVectorSubtract(moveDir, rightVec);
    }
    if (up)
    {
        moveDir = XMVectorAdd(moveDir, upVec);
    }
    if (down)
    {
        moveDir = XMVectorSubtract(moveDir, upVec);
    }

    // Normalize if moving
    if (XMVectorGetX(XMVector3LengthSq(moveDir)) > 0.0f)
    {
        moveDir = XMVector3Normalize(moveDir);
    }

    float speed = m_moveSpeed * (boost ? m_boostMultiplier : 1.0f);
    XMVECTOR pos = XMLoadFloat3(&m_position);
    pos = XMVectorAdd(pos, XMVectorScale(moveDir, speed * dt));
    XMStoreFloat3(&m_position, pos);
}

void FlyCamera::FrameTarget(const XMFLOAT3& target, float distance, float height)
{
    m_position = { target.x, target.y + height, target.z - distance };
    XMVECTOR dir = XMVector3Normalize(
        XMVectorSet(target.x - m_position.x, target.y - m_position.y,
                    target.z - m_position.z, 0.0f));
    float dx = XMVectorGetX(dir), dy = XMVectorGetY(dir), dz = XMVectorGetZ(dir);
    // Inverse of forward = (cos(pitch)sin(yaw), -sin(pitch), cos(pitch)cos(yaw)).
    m_yaw   = atan2f(dx, dz);
    m_pitch = -asinf(dy);
}

void FlyCamera::SetLookAt(const XMFLOAT3& eye, const XMFLOAT3& target)
{
    m_position = eye;
    XMVECTOR dir = XMVector3Normalize(
        XMVectorSet(target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.0f));
    float dx = XMVectorGetX(dir), dy = XMVectorGetY(dir), dz = XMVectorGetZ(dir);
    m_yaw   = atan2f(dx, dz);
    m_pitch = -asinf(dy);
}

XMMATRIX FlyCamera::GetViewMatrix() const
{
    XMVECTOR eye = XMLoadFloat3(&m_position);
    XMVECTOR forward =
        XMVectorSet(cosf(m_pitch) * sinf(m_yaw), -sinf(m_pitch), cosf(m_pitch) * cosf(m_yaw), 0.0f);
    XMVECTOR target = XMVectorAdd(eye, forward);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    return XMMatrixLookAtLH(eye, target, up);
}

XMMATRIX FlyCamera::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(m_fovY, m_aspectRatio, m_nearZ, m_farZ);
}
