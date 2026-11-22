// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <cwchar>
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t *command, int) {
    if (!std::wcsstr(command, L"--obs-output")) return 17;
    WNDCLASSW wc{};
    wc.hInstance = instance; wc.lpfnWndProc = window_proc; wc.lpszClassName = L"HorizonLockControls";
    if (!RegisterClassW(&wc)) return 18;
    HWND window = CreateWindowW(wc.lpszClassName, L"SteadyXR lifecycle test fixture", 0, 0, 0, 1, 1,
                                 nullptr, nullptr, instance, nullptr);
    if (!window) return 19;
    // Invisible isolated fixture; self-expiry protects against a failing test.
    SetTimer(window, 1, 15000, [](HWND w, UINT, UINT_PTR, DWORD) { PostMessageW(w, WM_CLOSE, 0, 0); });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
