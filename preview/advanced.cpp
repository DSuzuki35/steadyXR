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
    options.mirror_stretch =
        std::clamp(int(GetPrivateProfileIntW(L"Preview", L"MirrorStretchTenths", 0, ini.c_str())), 0, 1000) /
        10.;
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
    bool okay = WritePrivateProfileStringW(L"Preview", L"MirrorStretchTenths",
                                           std::to_wstring(tenths(options.mirror_stretch, 0, 100)).c_str(),
                                           ini.c_str()) != FALSE;
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
    HWND parent{}, window{}, profile{},
        render{}, symmetric{}, expansions[2]{}, expansion_labels[2]{}, explanation{}, stretch{}, stretch_label{};
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
        SetWindowTextW(stretch_label, (L"Edge stretch: " + decimal(options->mirror_stretch) + L" %").c_str());
        const double extra[]{pending.eyes[editing].left, pending.eyes[editing].right};
        for (unsigned i = 0; i < 2; ++i) {
            SetWindowTextW(expansion_labels[i],
                           ((i ? L"Extra right: " : L"Extra left: ") + decimal(extra[i]) + L" %").c_str());
            EnableWindow(expansions[i], pending.enabled);
        }
    }
    void load_controls() {
        SendMessageW(stretch, TBM_SETPOS, TRUE, tenths(options->mirror_stretch, 0, 100));
        SendMessageW(profile, CB_SETCURSEL, editing, 0);
        SendMessageW(render, BM_SETCHECK, pending.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(symmetric, BM_SETCHECK, pending.symmetric ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(expansions[0], TBM_SETPOS, TRUE, tenths(pending.eyes[editing].left, 0, 30));
        SendMessageW(expansions[1], TBM_SETPOS, TRUE, tenths(pending.eyes[editing].right, 0, 30));
        refresh_labels();
    }
    void synchronize_render() {
        if (pending.symmetric)
            pending.eyes[1 - editing] = {pending.eyes[editing].right, pending.eyes[editing].left};
    }
    void scroll() {
        options->mirror_stretch = SendMessageW(stretch, TBM_GETPOS, 0, 0) / 10.;
        pending.eyes[editing].left = SendMessageW(expansions[0], TBM_GETPOS, 0, 0) / 10.;
        pending.eyes[editing].right = SendMessageW(expansions[1], TBM_GETPOS, 0, 0) / 10.;
        synchronize_render();
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
        RECT r{0, 0, 744, 524};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"SteadyXR — Render & edge settings",
                                 style, 135, 120, r.right - r.left, r.bottom - r.top, parent, nullptr,
                                 cls.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        child(L"STATIC", L"Render & edge settings", 0, 24, 20, 690, 26, 0, true);
        child(L"STATIC",
              L"Adjust mirrored edges and request extra image area from your OpenXR game.", 0, 24,
              53, 695, 24);
        child(L"STATIC", L"Editing eye profile", 0, 24, 93, 192, 26);
        profile = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 240, 87, 474, 140, eye_id);
        SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Left eye — inner edge is right"));
        SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Right eye — inner edge is left"));
        stretch_label = child(L"STATIC", L"", 0, 24, 148, 205, 25);
        stretch = slider(stretch_id, 142, 0, 1000);
        child(L"STATIC",
              L"0% keeps mirroring; 100% strongly stretches pixels outside the captured image.\r\n"
              L"Mirror limit still caps the effect. Enable Mirror on the Framing page.",
              0, 24, 189, 690, 44);
        child(L"STATIC", L"OPENXR RENDER EXTENSION", 0, 24, 261, 690, 26, 0, true);
        render = child(L"BUTTON", L"Enable extra rendered area (next game launch)", BS_AUTOCHECKBOX, 24, 297,
                       440, 28, render_id);
        symmetric = child(L"BUTTON", L"Symmetric (both eyes)", BS_AUTOCHECKBOX, 475, 297,
                          239, 28, symmetric_id);
        for (unsigned i = 0; i < 2; ++i) {
            const int y = 340 + int(i) * 31;
            expansion_labels[i] = child(L"STATIC", L"", 0, 24, y, 205, 25);
            expansions[i] = slider(render_left_id + int(i), y - 5, 0, 300);
        }
        child(L"BUTTON", L"Both eyes inward: 10%", BS_PUSHBUTTON, 24, 414, 325, 36, inward_id);
        child(L"BUTTON", L"Save for next game launch", BS_PUSHBUTTON, 365, 414, 349, 36, save_render_id);
        explanation = child(L"STATIC",
                            L"Some games, including LMU, require symmetric render settings.\r\n"
                            L"Extra render area costs GPU performance. Restart the game to apply changes.",
                            0, 24, 461, 690, 44);
        theme->scale_window(window);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor))
            theme->fit_window(window, monitor.rcWork);
    }
    static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, message, w, l);
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
            case render_id:
                self->pending.enabled = SendMessageW(self->render, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->refresh_labels();
                return 0;
            case symmetric_id:
                self->pending.symmetric = SendMessageW(self->symmetric, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->synchronize_render();
                self->load_controls();
                return 0;
            case inward_id:
                self->pending = {};
                self->pending.enabled = true;
                self->pending.symmetric = true;
                self->pending.eyes[0].right = self->pending.eyes[1].left = 10;
                self->load_controls();
                return 0;
            case save_render_id: {
                const bool saved = write_render_settings(self->ini, self->pending);
                const std::wstring text =
                    saved ? L"Render settings saved. Restart your OpenXR game to apply them.\r\n"
                            L"For LMU, keep Symmetric enabled so both eyes have matching render widths."
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
void AdvancedSettings::refresh_live_controls() {
    if (impl_->window)
        impl_->load_controls();
}
void AdvancedSettings::reload(const std::wstring &ini) {
    impl_->ini = ini;
    impl_->pending = read_render_settings(ini);
    if (impl_->window)
        impl_->load_controls();
}
} // namespace vrst::preview
