#define UNICODE
#define _UNICODE
#include "advanced.hpp"
#include "render_settings.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwchar>

namespace vrst::preview {
namespace {
const wchar_t *sections[]{L"LeftEyeReserve", L"RightEyeReserve"};
const wchar_t *edge_keys[]{L"LeftTenths", L"RightTenths", L"TopTenths", L"BottomTenths"};
std::wstring decimal(double value) {
    wchar_t text[32]{};
    std::swprintf(text, 32, L"%.1f", value);
    return text;
}
int tenths(double value, double lo, double hi) {
    return int(std::lround((std::isfinite(value) ? std::clamp(value, lo, hi) : 0) * 10));
}
} // namespace
void read_edge_settings(const std::wstring &ini, Options &options) {
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto &reserve = options.edge_reserve[eye];
        reserve.enabled = GetPrivateProfileIntW(sections[eye], L"Enabled", 0, ini.c_str()) == 1;
        double *edges[]{&reserve.left, &reserve.right, &reserve.top, &reserve.bottom};
        for (unsigned i = 0; i < 4; ++i)
            *edges[i] = std::clamp(int(GetPrivateProfileIntW(sections[eye], edge_keys[i],
                                                             tenths(options.crop / 2, 0, 35), ini.c_str())),
                                   0, 350) /
                        10.;
    }
    options.vertical_offset =
        std::clamp(int(GetPrivateProfileIntW(L"Preview", L"VerticalOffsetTenths", 0, ini.c_str())), -1000,
                   1000) /
        10.;
}
bool write_edge_settings(const std::wstring &ini, const Options &options) {
    bool okay = true;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto &reserve = options.edge_reserve[eye];
        okay = WritePrivateProfileStringW(sections[eye], L"Enabled", reserve.enabled ? L"1" : L"0",
                                          ini.c_str()) &&
               okay;
        const double edges[]{reserve.left, reserve.right, reserve.top, reserve.bottom};
        for (unsigned i = 0; i < 4; ++i)
            okay =
                WritePrivateProfileStringW(sections[eye], edge_keys[i],
                                           std::to_wstring(tenths(edges[i], 0, 35)).c_str(), ini.c_str()) &&
                okay;
    }
    return WritePrivateProfileStringW(L"Preview", L"VerticalOffsetTenths",
                                      std::to_wstring(tenths(options.vertical_offset, -100, 100)).c_str(),
                                      ini.c_str()) &&
           okay;
}

struct AdvancedSettings::Impl {
    HWND parent{}, window{}, profile{}, separate{}, edges[4]{}, labels[4]{}, vertical{}, vertical_label{},
        render{}, expansions[2]{}, expansion_labels[2]{}, explanation{};
    Theme *theme{};
    Options *options{};
    std::wstring ini;
    RenderSettings pending;
    unsigned editing{};
    std::function<void()> changed;
    std::function<void(std::wstring)> notice;

    HWND child(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int width, int height,
               int id = 0, bool heading = false) {
        if (std::wcscmp(kind, L"STATIC"))
            style |= WS_TABSTOP;
        auto h = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | theme->style(kind, style), x, y,
                                 width, height, window, reinterpret_cast<HMENU>(INT_PTR(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        theme->control(h, kind, heading);
        return h;
    }
    HWND slider(int id, int y, int min, int max, int width = 476) {
        auto h = child(TRACKBAR_CLASSW, L"", TBS_HORZ, 238, y, width, 32, id);
        SendMessageW(h, TBM_SETRANGEMIN, FALSE, min);
        SendMessageW(h, TBM_SETRANGEMAX, TRUE, max);
        SendMessageW(h, TBM_SETLINESIZE, 0, 1);
        SendMessageW(h, TBM_SETPAGESIZE, 0, 10);
        return h;
    }
    void refresh_labels() {
        const auto &reserve = options->edge_reserve[editing];
        const double values[]{reserve.left, reserve.right, reserve.top, reserve.bottom};
        const wchar_t *names[]{L"Left reserve", L"Right reserve", L"Top reserve", L"Bottom reserve"};
        for (unsigned i = 0; i < 4; ++i) {
            SetWindowTextW(labels[i], (std::wstring(names[i]) + L": " + decimal(values[i]) + L" %").c_str());
            EnableWindow(edges[i], reserve.enabled);
        }
        SetWindowTextW(vertical_label, (L"Y offset: " + decimal(options->vertical_offset) + L" %").c_str());
        const double extra[]{pending.eyes[editing].left, pending.eyes[editing].right};
        for (unsigned i = 0; i < 2; ++i) {
            SetWindowTextW(expansion_labels[i],
                           ((i ? L"Extra right: " : L"Extra left: ") + decimal(extra[i]) + L" %").c_str());
            EnableWindow(expansions[i], pending.enabled);
        }
    }
    void load_controls() {
        SendMessageW(profile, CB_SETCURSEL, editing, 0);
        const auto &reserve = options->edge_reserve[editing];
        SendMessageW(separate, BM_SETCHECK, reserve.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        const double values[]{reserve.left, reserve.right, reserve.top, reserve.bottom};
        for (unsigned i = 0; i < 4; ++i)
            SendMessageW(edges[i], TBM_SETPOS, TRUE, tenths(values[i], 0, 35));
        SendMessageW(vertical, TBM_SETPOS, TRUE, tenths(options->vertical_offset, -100, 100));
        SendMessageW(render, BM_SETCHECK, pending.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(expansions[0], TBM_SETPOS, TRUE, tenths(pending.eyes[editing].left, 0, 30));
        SendMessageW(expansions[1], TBM_SETPOS, TRUE, tenths(pending.eyes[editing].right, 0, 30));
        refresh_labels();
    }
    void scroll() {
        auto &reserve = options->edge_reserve[editing];
        double *values[]{&reserve.left, &reserve.right, &reserve.top, &reserve.bottom};
        for (unsigned i = 0; i < 4; ++i)
            *values[i] = SendMessageW(edges[i], TBM_GETPOS, 0, 0) / 10.;
        options->vertical_offset = SendMessageW(vertical, TBM_GETPOS, 0, 0) / 10.;
        pending.eyes[editing].left = SendMessageW(expansions[0], TBM_GETPOS, 0, 0) / 10.;
        pending.eyes[editing].right = SendMessageW(expansions[1], TBM_GETPOS, 0, 0) / 10.;
        refresh_labels();
        changed();
    }
    void create() {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"HorizonLockAdvanced";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpfnWndProc = proc;
        RegisterClassW(&cls);
        RECT r{0, 0, 744, 754};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"SteadyXR — Render & edge settings",
                                 style, 135, 120, r.right - r.left, r.bottom - r.top, parent, nullptr,
                                 cls.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        child(L"STATIC", L"FRAMING / EXTRA RENDER AREA", 0, 24, 20, 690, 26, 0, true);
        child(L"STATIC",
              L"Adjust the stream view. The full captured image remains available for stabilization.", 0, 24,
              53, 695, 24);
        child(L"STATIC", L"Editing eye profile", 0, 24, 93, 192, 26);
        profile = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 240, 87, 474, 140, eye_id);
        SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Left eye — inner edge is right"));
        SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Right eye — inner edge is left"));
        separate = child(L"BUTTON", L"Use separate stabilization reserves for this eye", BS_AUTOCHECKBOX, 24,
                         133, 690, 28, separate_id);
        for (unsigned i = 0; i < 4; ++i) {
            const int y = 178 + int(i) * 36;
            labels[i] = child(L"STATIC", L"", 0, 24, y, 205, 25);
            edges[i] = slider(left_id + int(i), y - 5, 0, 350);
        }
        vertical_label = child(L"STATIC", L"", 0, 24, 338, 205, 25);
        vertical = slider(vertical_id, 331, -1000, 1000, 387);
        child(L"BUTTON", L"Center", BS_PUSHBUTTON, 636, 330, 78, 31, center_id);
        child(L"STATIC",
              L"Y offset shifts the view within the full image: negative = up, positive = down.\r\n"
              L"Uses hidden crop pixels; shifting leaves less stabilization room on that edge.",
              0, 24, 373, 690, 44);
        child(L"STATIC", L"OPENXR RENDER EXTENSION", 0, 24, 439, 690, 26, 0, true);
        render = child(L"BUTTON", L"Enable extra rendered area (next game launch)", BS_AUTOCHECKBOX, 24, 475,
                       690, 28, render_id);
        for (unsigned i = 0; i < 2; ++i) {
            const int y = 522 + int(i) * 36;
            expansion_labels[i] = child(L"STATIC", L"", 0, 24, y, 205, 25);
            expansions[i] = slider(render_left_id + int(i), y - 5, 0, 300);
        }
        child(L"BUTTON", L"Both eyes inward: 10%", BS_PUSHBUTTON, 24, 602, 325, 36, inward_id);
        child(L"BUTTON", L"Save for next game launch", BS_PUSHBUTTON, 365, 602, 349, 36, save_render_id);
        explanation =
            child(L"STATIC",
                  L"Adds real render area and requests wider textures. GPU cost increases.\r\nRequires the "
                  L"0.6 connection and an OpenXR game restart after saving.\r\nRender settings stay unsaved "
                  L"until you click Save; edge and Y sliders are live.",
                  0, 24, 658, 690, 76);
    }
    static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, message, w, l);
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            auto dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            RECT r{};
            GetClientRect(h, &r);
            SetDCBrushColor(dc, RGB(24, 31, 42));
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SetDCBrushColor(dc, RGB(53, 66, 82));
            RECT line{24, 425, 714, 426};
            FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (message == WM_PAINT)
                EndPaint(h, &ps);
            return 0;
        }
        LRESULT themed{};
        if (self->theme->message(h, message, w, l, themed))
            return themed;
        switch (message) {
        case WM_HSCROLL:
            self->scroll();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case eye_id:
                if (HIWORD(w) == CBN_SELCHANGE) {
                    self->editing = std::clamp(int(SendMessageW(self->profile, CB_GETCURSEL, 0, 0)), 0, 1);
                    self->load_controls();
                }
                return 0;
            case separate_id:
                self->options->edge_reserve[self->editing].enabled =
                    SendMessageW(self->separate, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->refresh_labels();
                self->changed();
                return 0;
            case center_id:
                SendMessageW(self->vertical, TBM_SETPOS, TRUE, 0);
                self->scroll();
                return 0;
            case render_id:
                self->pending.enabled = SendMessageW(self->render, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->refresh_labels();
                return 0;
            case inward_id:
                self->pending = {};
                self->pending.enabled = true;
                self->pending.eyes[0].right = self->pending.eyes[1].left = 10;
                self->load_controls();
                return 0;
            case save_render_id: {
                const bool saved = write_render_settings(self->ini, self->pending);
                const std::wstring text =
                    saved ? L"Render settings saved. Restart iRacing to apply them.\r\nUse Upgrade "
                            L"connection in the main window if an older layer is installed."
                          : L"Could not save render settings. Check access to the preview folder.";
                SetWindowTextW(self->explanation, text.c_str());
                self->notice(text);
                return 0;
            }
            }
            break;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_NCDESTROY:
            self->window = nullptr;
            break;
        }
        return DefWindowProcW(h, message, w, l);
    }
};
AdvancedSettings::AdvancedSettings() : impl_(std::make_unique<Impl>()) {}
AdvancedSettings::~AdvancedSettings() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void AdvancedSettings::initialize(HWND parent, Theme &theme, Options &options, const std::wstring &ini,
                                  std::function<void()> changed, std::function<void(std::wstring)> notice) {
    auto &s = *impl_;
    s.parent = parent;
    s.theme = &theme;
    s.options = &options;
    s.ini = ini;
    s.changed = std::move(changed);
    s.notice = std::move(notice);
}
void AdvancedSettings::open(unsigned eye, bool visible) {
    auto &s = *impl_;
    if (!s.window)
        s.pending = read_render_settings(s.ini);
    s.editing = std::min(eye, 1u);
    if (!s.window)
        s.create();
    if (!s.window)
        return;
    s.load_controls();
    if (visible) {
        ShowWindow(s.window, SW_SHOW);
        SetForegroundWindow(s.window);
    }
}
HWND AdvancedSettings::handle() const {
    return impl_->window;
}
bool AdvancedSettings::route(MSG &msg) {
    return impl_->window && IsWindowVisible(impl_->window) && IsDialogMessageW(impl_->window, &msg);
}
RenderSettings AdvancedSettings::draft() const {
    return impl_->window ? impl_->pending : read_render_settings(impl_->ini);
}
void AdvancedSettings::reload(const std::wstring &ini) {
    impl_->ini = ini;
    impl_->pending = read_render_settings(ini);
    if (impl_->window)
        impl_->load_controls();
}
} // namespace vrst::preview
