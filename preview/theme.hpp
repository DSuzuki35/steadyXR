#pragma once
#include <array>
#include <vector>
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
    // Capture the logical layout once, then scale controls and text without rounding drift.
    void scale_window(HWND window, UINT dpi = 0);
    // Limit the viewport while preserving the full logical form through native scrollbars.
    // Call after scale_window; focus and resize messages keep the form reachable thereafter.
    void fit_window(HWND window, const RECT &work_area);
    void surface(HWND window, HDC dc, RECT card);
    bool message(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result);

  private:
    HFONT title_{}, heading_{}, strong_{}, caption_{};
    HBRUSH background_{}, panel_{}, field_{};
    std::array<HBRUSH, 4> shell_brushes_{};
    struct Placement {
        HWND window;
        RECT bounds;
    };
    struct Layout {
        HWND window;
        int width, height;
        std::vector<Placement> children;
    };
    struct Fonts {
        UINT dpi;
        HFONT body, heading, title, strong;
    };
    std::vector<Layout> layouts_;
    std::vector<Fonts> fonts_;
    void scroll_to(HWND window, int x, int y);
    void update_scroll_range(HWND window);
    void reveal_control(HWND window, HWND child);
};
} // namespace vrst::preview
