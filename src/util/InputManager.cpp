#include "InputManager.h"
#include <cstring>

void InputManager::BeginFrame()
{
    memset(m_keysPressed, 0, sizeof(m_keysPressed));
    memset(m_keysReleased, 0, sizeof(m_keysReleased));
    memset(m_mouseButtonsPressed, 0, sizeof(m_mouseButtonsPressed));
    memset(m_mouseButtonsReleased, 0, sizeof(m_mouseButtonsReleased));
    memset(m_keysConsumed, 0, sizeof(m_keysConsumed));
    memset(m_mouseButtonsConsumed, 0, sizeof(m_mouseButtonsConsumed));
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
    m_mouseWheelDelta = 0;
    m_mouseMoveConsumed = false;
    m_mouseWheelConsumed = false;
}

void InputManager::OnKeyDown(int key)
{
    if (key >= 0 && key < kMaxKeys)
    {
        if (!m_keys[key])
        {
            m_keysPressed[key] = true;
        }
        m_keys[key] = true;
    }
}

void InputManager::OnKeyUp(int key)
{
    if (key >= 0 && key < kMaxKeys)
    {
        m_keys[key] = false;
        m_keysReleased[key] = true;
    }
}

void InputManager::OnMouseButtonDown(int button)
{
    if (button >= 0 && button < kMaxMouseButtons)
    {
        if (!m_mouseButtons[button])
        {
            m_mouseButtonsPressed[button] = true;
        }
        m_mouseButtons[button] = true;
    }
}

void InputManager::OnMouseButtonUp(int button)
{
    if (button >= 0 && button < kMaxMouseButtons)
    {
        m_mouseButtons[button] = false;
        m_mouseButtonsReleased[button] = true;
    }
}

void InputManager::OnMouseMove(int x, int y)
{
    if (m_hasMousePosition)
    {
        m_mouseDeltaX += x - m_mouseX;
        m_mouseDeltaY += y - m_mouseY;
    }
    m_mouseX = x;
    m_mouseY = y;
    m_hasMousePosition = true;
}

void InputManager::OnMouseWheel(int delta)
{
    m_mouseWheelDelta += delta;
}

bool InputManager::IsKeyDown(int key) const
{
    return key >= 0 && key < kMaxKeys && m_keys[key] && !m_keysConsumed[key];
}

bool InputManager::IsKeyPressed(int key) const
{
    return key >= 0 && key < kMaxKeys && m_keysPressed[key] && !m_keysConsumed[key];
}

bool InputManager::IsKeyReleased(int key) const
{
    return key >= 0 && key < kMaxKeys && m_keysReleased[key] && !m_keysConsumed[key];
}

bool InputManager::IsMouseButtonDown(int button) const
{
    return button >= 0 && button < kMaxMouseButtons && m_mouseButtons[button] &&
           !m_mouseButtonsConsumed[button];
}

bool InputManager::IsMouseButtonPressed(int button) const
{
    return button >= 0 && button < kMaxMouseButtons && m_mouseButtonsPressed[button] &&
           !m_mouseButtonsConsumed[button];
}

bool InputManager::IsMouseButtonReleased(int button) const
{
    return button >= 0 && button < kMaxMouseButtons && m_mouseButtonsReleased[button] &&
           !m_mouseButtonsConsumed[button];
}

void InputManager::ConsumeKey(int key)
{
    if (key >= 0 && key < kMaxKeys)
    {
        m_keysConsumed[key] = true;
    }
}

void InputManager::ConsumeMouseButton(int button)
{
    if (button >= 0 && button < kMaxMouseButtons)
    {
        m_mouseButtonsConsumed[button] = true;
    }
}

void InputManager::ConsumeMouseMove()
{
    m_mouseMoveConsumed = true;
}

void InputManager::ConsumeMouseWheel()
{
    m_mouseWheelConsumed = true;
}

void InputManager::ConsumeAllKeys()
{
    memset(m_keysConsumed, true, sizeof(m_keysConsumed));
}

void InputManager::ConsumeAllMouseButtons()
{
    memset(m_mouseButtonsConsumed, true, sizeof(m_mouseButtonsConsumed));
}

void InputManager::ConsumeAllMouse()
{
    ConsumeAllMouseButtons();
    ConsumeMouseMove();
    ConsumeMouseWheel();
}
