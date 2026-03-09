#pragma once

class InputManager
{
public:
    // Call at the start of each frame, before processing window messages.
    // Clears per-frame deltas, events, and consumption flags.
    void BeginFrame();

    // Raw event recording — called from App::OnWindowMessage.
    void OnKeyDown(int key);
    void OnKeyUp(int key);
    void OnMouseButtonDown(int button);
    void OnMouseButtonUp(int button);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta);

    // State queries — return false / zero if the corresponding input has been consumed.
    bool IsKeyDown(int key) const;
    bool IsKeyPressed(int key) const;
    bool IsKeyReleased(int key) const;
    bool IsMouseButtonDown(int button) const;
    bool IsMouseButtonPressed(int button) const;
    bool IsMouseButtonReleased(int button) const;
    int GetMouseDeltaX() const { return m_mouseMoveConsumed ? 0 : m_mouseDeltaX; }
    int GetMouseDeltaY() const { return m_mouseMoveConsumed ? 0 : m_mouseDeltaY; }
    int GetMouseWheelDelta() const { return m_mouseWheelConsumed ? 0 : m_mouseWheelDelta; }

    // Raw position (not consumable — always reflects true cursor location).
    int GetMouseX() const { return m_mouseX; }
    int GetMouseY() const { return m_mouseY; }

    // Per-key / per-button consumption.
    void ConsumeKey(int key);
    void ConsumeMouseButton(int button);
    void ConsumeMouseMove();
    void ConsumeMouseWheel();

    // Convenience — consume all keys or all mouse input at once.
    void ConsumeAllKeys();
    void ConsumeAllMouseButtons();
    void ConsumeAllMouse();

private:
    static constexpr int kMaxKeys = 256;
    static constexpr int kMaxMouseButtons = 5;

    // Persistent key/button state
    bool m_keys[kMaxKeys] = {};
    bool m_mouseButtons[kMaxMouseButtons] = {};

    // Per-frame transition events
    bool m_keysPressed[kMaxKeys] = {};
    bool m_keysReleased[kMaxKeys] = {};
    bool m_mouseButtonsPressed[kMaxMouseButtons] = {};
    bool m_mouseButtonsReleased[kMaxMouseButtons] = {};

    // Mouse position and per-frame deltas
    int m_mouseX = 0;
    int m_mouseY = 0;
    int m_mouseDeltaX = 0;
    int m_mouseDeltaY = 0;
    int m_mouseWheelDelta = 0;
    bool m_hasMousePosition = false;

    // Per-key / per-button consumption flags (reset each frame)
    bool m_keysConsumed[kMaxKeys] = {};
    bool m_mouseButtonsConsumed[kMaxMouseButtons] = {};
    bool m_mouseMoveConsumed = false;
    bool m_mouseWheelConsumed = false;
};
