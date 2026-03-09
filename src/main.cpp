#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include "App.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LPCTSTR g_windowClassName = TEXT("AidocWindowClass");
static LPCTSTR g_windowTitle = TEXT("aidoc - D3D12 Hello Triangle");
static bool g_running = true;
static App g_app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    switch (msg)
    {
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            g_running = false;
            PostQuitMessage(0);
        }
        // fall through
    default:
        g_app.OnWindowMessage(hwnd, msg, wParam, lParam);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = g_windowClassName;
    RegisterClassEx(&wc);

    RECT windowRect = {0, 0, static_cast<LONG>(App::WindowWidth),
                       static_cast<LONG>(App::WindowHeight)};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd =
        CreateWindow(wc.lpszClassName, g_windowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                     CW_USEDEFAULT, windowRect.right - windowRect.left,
                     windowRect.bottom - windowRect.top, nullptr, nullptr, hInstance, nullptr);

    g_app.Init(hwnd);

    ShowWindow(hwnd, nCmdShow);

    LARGE_INTEGER freq, prevTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prevTime);

    MSG msg = {};
    while (g_running)
    {
        g_app.BeginInputFrame();
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_running)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - prevTime.QuadPart) /
                       static_cast<float>(freq.QuadPart);
            prevTime = now;

            g_app.Update(dt);
            g_app.Render();
        }
    }

    g_app.Cleanup();
    return 0;
}
