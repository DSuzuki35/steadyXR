#include "theme.hpp"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Real native trackbars on a hidden parent: the test never activates a window.
struct Slider {
    vrst::preview::Theme theme;
    HWND parent{}, window{};
    unsigned prepaints{}, item_paints{}, scrolls{};
    static constexpr int width = 385, height = 40;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Slider *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Slider *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_NOTIFY) {
            const auto &draw = *reinterpret_cast<NMCUSTOMDRAW *>(l);
            if (draw.hdr.code == NM_CUSTOMDRAW) {
                self->prepaints += draw.dwDrawStage == CDDS_PREPAINT;
                self->item_paints += draw.dwDrawStage == CDDS_ITEMPREPAINT;
            }
            LRESULT result{};
            if (self->theme.message(window, message, w, l, result))
                return result;
        }
        if (self && message == WM_HSCROLL)
            ++self->scrolls;
        return DefWindowProcW(window, message, w, l);
    }

    Slider() {
        theme.initialize();
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"HorizonLockSliderPaintTest";
        RegisterClassW(&type);
        parent = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"", WS_POPUP, 0, 0, width, height,
                                 nullptr, nullptr, type.hInstance, this);
        check(parent != nullptr, "create hidden slider parent");
        window = CreateWindowW(TRACKBAR_CLASSW, L"", theme.style(TRACKBAR_CLASSW, WS_CHILD | WS_VISIBLE), 0,
                               0, width, height, parent, nullptr, type.hInstance, nullptr);
        check(window != nullptr, "create native slider");
        theme.control(window, TRACKBAR_CLASSW);
        SendMessageW(window, TBM_SETRANGEMIN, FALSE, -1000);
        SendMessageW(window, TBM_SETRANGEMAX, FALSE, 1000);
        SendMessageW(window, TBM_SETLINESIZE, 0, 1);
        SendMessageW(window, TBM_SETPAGESIZE, 0, 100);
        position(-500);
    }
    ~Slider() {
        if (parent)
            DestroyWindow(parent);
    }
    void position(int value) {
        SendMessageW(window, TBM_SETPOS, TRUE, value);
    }
    int position() const {
        return static_cast<int>(SendMessageW(window, TBM_GETPOS, 0, 0));
    }
    RECT rectangle(UINT message) const {
        RECT rectangle{};
        SendMessageW(window, message, 0, reinterpret_cast<LPARAM>(&rectangle));
        return rectangle;
    }
};

struct Canvas {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    Canvas() {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Slider::width;
        info.bmiHeader.biHeight = -Slider::height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *pixels{};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        check(dc && bitmap, "create slider paint DIB");
        previous = SelectObject(dc, bitmap);
        clear();
    }
    ~Canvas() {
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void clear() {
        SetDCBrushColor(dc, RGB(255, 0, 255));
        RECT rect{0, 0, Slider::width, Slider::height};
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void paint(const Slider &slider) {
        SendMessageW(slider.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    }
    COLORREF pixel(int x, int y) const {
        return GetPixel(dc, x, y);
    }
    std::vector<COLORREF> pixels() const {
        std::vector<COLORREF> result;
        for (int y = 0; y < Slider::height; ++y)
            for (int x = 0; x < Slider::width; ++x)
                result.push_back(pixel(x, y));
        return result;
    }
};

void repeated_native_paint_keeps_track_and_thumb() {
    Slider slider;
    Canvas canvas;
    canvas.paint(slider);
    const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
    const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
    const auto y = (channel.top + channel.bottom) / 2;
    const auto background = canvas.pixel(0, Slider::height - 1);
    check(background == RGB(26, 27, 31), "slider uses the approved dark surface");
    check(canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2) == RGB(112, 89, 221),
          "slider uses the approved violet accent");
    check(canvas.pixel(channel.right - 2, y) != background, "first native paint draws the track");
    check(canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2) != background,
          "first native paint draws the thumb");
    const auto complete = canvas.pixels();
    slider.prepaints = slider.item_paints = 0;
    canvas.paint(slider);
    check(slider.prepaints == 1, "native repeated print reaches custom prepaint");
    check(canvas.pixels() == complete,
          "a native repaint with no dirty items must preserve the complete track and thumb");
    check(slider.item_paints == 0, "complete custom paint must not depend on native item callbacks");
}

void clipped_native_paint_restores_track_under_old_thumb() {
    Slider slider;
    Canvas incremental;
    incremental.paint(slider);
    const auto before = incremental.pixels();
    const auto old_thumb = slider.rectangle(TBM_GETTHUMBRECT);
    slider.position(500);
    const auto new_thumb = slider.rectangle(TBM_GETTHUMBRECT);
    RECT damage{};
    UnionRect(&damage, &old_thumb, &new_thumb);
    const int saved = SaveDC(incremental.dc);
    IntersectClipRect(incremental.dc, damage.left, damage.top, damage.right, damage.bottom);
    incremental.paint(slider);
    RestoreDC(incremental.dc, saved);
    const auto clipped = incremental.pixels();
    Canvas complete;
    // Native print has no dirty items now; it must still produce a whole slider.
    complete.paint(slider);
    const auto expected = complete.pixels();
    for (int y = 0; y < Slider::height; ++y) {
        for (int x = 0; x < Slider::width; ++x) {
            const auto index = static_cast<std::size_t>(y * Slider::width + x);
            const bool damaged = x >= damage.left && x < damage.right && y >= damage.top && y < damage.bottom;
            check(clipped[index] == (damaged ? expected[index] : before[index]),
                  "partial native paint restores damaged pixels and honors the HDC clip");
        }
    }
    const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
    check(incremental.pixel((old_thumb.left + old_thumb.right) / 2, (channel.top + channel.bottom) / 2) !=
              incremental.pixel(0, Slider::height - 1),
          "moving the thumb must restore its old track segment");
}

void signed_range_fill_tracks_native_position() {
    Slider slider;
    Canvas canvas;
    for (const int value : {-1000, -500, 0, 500, 1000}) {
        slider.position(value);
        canvas.paint(slider);
        const auto channel = slider.rectangle(TBM_GETCHANNELRECT);
        const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
        const auto y = (channel.top + channel.bottom) / 2;
        const auto foreground = canvas.pixel((thumb.left + thumb.right) / 2, thumb.top + 2);
        const auto background = canvas.pixel(0, Slider::height - 1);
        for (const int fraction : {1, 3, 5, 7}) {
            const int x = channel.left + (channel.right - channel.left) * fraction / 8;
            const int sample_value = -1000 + fraction * 250;
            check(canvas.pixel(x, y) != background, "signed slider always retains its full track");
            check((canvas.pixel(x, y) == foreground) == (sample_value < value),
                  "negative, zero and positive positions fill the correct signed range fraction");
        }
    }
    EnableWindow(slider.window, FALSE);
    canvas.paint(slider);
    const auto disabled = canvas.pixels();
    canvas.paint(slider);
    check(canvas.pixels() == disabled, "disabled track and thumb survive repeated paints");
}

void painting_preserves_native_input_and_notifications() {
    Slider slider;
    Canvas canvas;
    canvas.paint(slider);
    SendMessageW(slider.window, WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(slider.window, WM_KEYUP, VK_RIGHT, 0);
    check(slider.position() == -499, "custom painted slider retains native arrow-key movement");
    check(slider.scrolls != 0, "native keyboard changes still notify the parent");
    canvas.paint(slider);
    const auto thumb = slider.rectangle(TBM_GETTHUMBRECT);
    const auto x = (thumb.left + thumb.right) / 2;
    const auto y = (thumb.top + thumb.bottom) / 2;
    const auto notifications = slider.scrolls;
    SendMessageW(slider.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(slider.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 50, y));
    SendMessageW(slider.window, WM_LBUTTONUP, 0, MAKELPARAM(x + 50, y));
    check(slider.position() > -499, "custom painted slider retains native thumb dragging");
    check(slider.scrolls > notifications, "native drag changes still notify the parent");
}

void dpi_change_scales_controls_and_preserves_input() {
    Slider slider;
    const auto text = CreateWindowW(L"STATIC", L"DPI text", WS_CHILD, 0, 0, 100, 24, slider.parent, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    slider.theme.control(text, L"STATIC");
    RECT suggested{0, 0, 578, 60};
    LRESULT result{};
    check(slider.theme.message(slider.parent, WM_DPICHANGED, MAKELONG(144, 144),
                               reinterpret_cast<LPARAM>(&suggested), result),
          "theme handles per-monitor DPI changes");
    RECT bounds{};
    GetClientRect(slider.window, &bounds);
    check(bounds.right == 578 && bounds.bottom == 60, "DPI change scales native controls to 150 percent");
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(text, WM_GETFONT, 0, 0)), sizeof(font), &font);
    check(font.lfHeight == -24, "DPI change scales native control text");
    check(slider.position() == -500, "DPI change preserves slider value");
    suggested = {0, 0, Slider::width, Slider::height};
    slider.theme.message(slider.parent, WM_DPICHANGED, MAKELONG(96, 96), reinterpret_cast<LPARAM>(&suggested),
                         result);
    GetClientRect(slider.window, &bounds);
    check(bounds.right == Slider::width && bounds.bottom == Slider::height,
          "returning to 100 percent restores the original layout without drift");
}
} // namespace

int main() {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        check(InitCommonControlsEx(&controls), "initialize native trackbars");
        const auto foreground = GetForegroundWindow();
        repeated_native_paint_keeps_track_and_thumb();
        clipped_native_paint_restores_track_under_old_thumb();
        signed_range_fill_tracks_native_position();
        painting_preserves_native_input_and_notifications();
        dpi_change_scales_controls_and_preserves_input();
        check(GetForegroundWindow() == foreground, "slider tests never activate a window");
        std::cout << "Native slider painting and input tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
