#pragma once
#include <windows.h>

namespace vrst::preview {
// Native controls retain their keyboard, focus and accessibility behavior.
// Only their painting is customized; no rendering work runs on a UI timer.
class Theme {
  public:
    ~Theme();
    void initialize();
    HFONT body{};
    HICON icon{}, small_icon{};
    DWORD style(const wchar_t *kind, DWORD value) const;
    void control(HWND window, const wchar_t *kind, bool heading = false);
    void titlebar(HWND window);
    bool message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result);

  private:
    HFONT title_{}, heading_{};
    HBRUSH background_{}, panel_{}, field_{};
};
} // namespace vrst::preview
