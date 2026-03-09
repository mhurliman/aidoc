#include "OrbitCamera.h"
#include "InputManager.h"
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace DirectX;

void OrbitCamera::SetPerspective(float fovY, float aspectRatio, float nearZ, float farZ)
{
    m_fovY = fovY;
    m_aspectRatio = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;
}

void OrbitCamera::SetDistance(float distance)
{
    m_distance = distance;
}

void OrbitCamera::SetOrbitSensitivity(float sensitivity)
{
    m_sensitivity = sensitivity;
}

void OrbitCamera::SetZoomSpeed(float speed)
{
    m_zoomSpeed = speed;
}

void OrbitCamera::SetDistanceLimits(float minDist, float maxDist)
{
    m_minDistance = minDist;
    m_maxDistance = maxDist;
}

void OrbitCamera::Update(float dt, const InputManager& input)
{
    // All queries automatically return false/zero if consumed,
    // so no blanket early-out needed.
    float dx = static_cast<float>(input.GetMouseDeltaX());
    float dy = static_cast<float>(input.GetMouseDeltaY());

    // Left mouse button: orbit
    if (input.IsMouseButtonDown(0))
    {
        m_yaw += dx * m_sensitivity;
        m_pitch += dy * m_sensitivity;

        const float limit = XM_PIDIV2 - 0.01f;
        if (m_pitch > limit)
        {
            m_pitch = limit;
        }
        if (m_pitch < -limit)
        {
            m_pitch = -limit;
        }
    }

    // Right mouse button: pan
    if (input.IsMouseButtonDown(1))
    {
        XMVECTOR right = XMVectorSet(cosf(m_yaw), 0.0f, -sinf(m_yaw), 0.0f);
        XMVECTOR forward = XMVectorSet(cosf(m_pitch) * sinf(m_yaw), sinf(m_pitch),
                                       cosf(m_pitch) * cosf(m_yaw), 0.0f);
        XMVECTOR up = XMVector3Cross(forward, right);

        float panScale = m_panSpeed * m_distance;
        XMVECTOR offset = XMVectorScale(right, -dx * panScale) + XMVectorScale(up, dy * panScale);

        XMVECTOR target = XMLoadFloat3(&m_target);
        target = XMVectorAdd(target, offset);
        XMStoreFloat3(&m_target, target);
    }

    // Mouse wheel: zoom
    int wheel = input.GetMouseWheelDelta();
    if (wheel != 0)
    {
        m_distance -= static_cast<float>(wheel) * m_zoomSpeed / static_cast<float>(WHEEL_DELTA);
        if (m_distance < m_minDistance)
        {
            m_distance = m_minDistance;
        }
        if (m_distance > m_maxDistance)
        {
            m_distance = m_maxDistance;
        }
    }
}

XMMATRIX OrbitCamera::GetViewMatrix() const
{
    float x = m_distance * cosf(m_pitch) * sinf(m_yaw);
    float y = m_distance * sinf(m_pitch);
    float z = m_distance * cosf(m_pitch) * cosf(m_yaw);

    XMVECTOR target = XMLoadFloat3(&m_target);
    XMVECTOR eye = XMVectorAdd(target, XMVectorSet(x, y, z, 0.0f));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    return XMMatrixLookAtLH(eye, target, up);
}

XMMATRIX OrbitCamera::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(m_fovY, m_aspectRatio, m_nearZ, m_farZ);
}
