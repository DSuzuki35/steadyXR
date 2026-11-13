#include "output_fallback_settings.hpp"
#include "theme.hpp"
#include <algorithm>
#include <cwchar>
#include <iterator>
#include <utility>
#include <vector>

namespace vrst::preview {
namespace {
constexpr wchar_t section[] = L"OutputFallback";
enum ControlId { mode_id = 7301, monitor_id, cockpit_id };
struct MonitorChoice {
    std::wstring name, label;
};
BOOL CALLBACK add_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        std::wstring name = info.szDevice;
        const auto display = name.substr(name.find_last_of(L'\\') + 1);
        const auto &r = info.rcMonitor;
        auto label = display + L"  (" + std::to_wstring(r.right - r.left) + L" x " +
                     std::to_wstring(r.bottom - r.top) +
                     (info.dwFlags & MONITORINFOF_PRIMARY ? L", primary)" : L")");
        reinterpret_cast<std::vector<MonitorChoice> *>(data)->push_back({name, label});
    }
    return TRUE;
}
} // namespace

OutputFallbackSettings read_output_fallback_settings(const std::wstring &ini) {
    OutputFallbackSettings s;
    if (ini.empty())
        return s;
    wchar_t mode[32]{}, device[CCHDEVICENAME]{};
    GetPrivateProfileStringW(section, L"Mode", L"", mode, DWORD(std::size(mode)), ini.c_str());
    if (!std::wcscmp(mode, L"hide"))
        s.mode = FallbackMode::hide;
    else if (!std::wcscmp(mode, L"monitor"))
        s.mode = FallbackMode::monitor;
    s.cockpit_only = GetPrivateProfileIntW(section, L"CockpitOnly", 0, ini.c_str()) == 1;
    GetPrivateProfileStringW(section, L"Monitor", L"", device, DWORD(std::size(device)), ini.c_str());
    s.monitor_name = device;
    return s;
}

bool write_output_fallback_settings(const std::wstring &ini, const OutputFallbackSettings &s) {
    if (ini.empty())
        return false;
    const auto mode = s.mode == FallbackMode::monitor ? L"monitor"
                      : s.mode == FallbackMode::hide  ? L"hide"
                                                      : L"waiting";
    return WritePrivateProfileStringW(section, L"Monitor", s.monitor_name.c_str(), ini.c_str()) &&
           WritePrivateProfileStringW(section, L"CockpitOnly", s.cockpit_only ? L"1" : L"0", ini.c_str()) &&
           WritePrivateProfileStringW(section, L"Mode", mode, ini.c_str());
}

struct OutputFallbackDialog::Impl {
    Theme *theme{};
    HWND window{}, mode{}, monitor_label{}, monitor{}, mode_help{}, cockpit{};
    std::vector<MonitorChoice> choices;
    std::function<void(OutputFallbackSettings)> applied;

    void refresh_monitors(const std::wstring &selected) {
        std::vector<MonitorChoice> next{{L"", L"Primary display (follow Windows)"}};
        EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&next));
        auto match = std::find_if(next.begin(), next.end(), [&](const auto &choice) {
            return !_wcsicmp(choice.name.c_str(), selected.c_str());
        });
        auto index = std::distance(next.begin(), match);
        if (match == next.end())
            next.push_back({selected, selected + L" (disconnected)"});
        choices = std::move(next);
        SendMessageW(monitor, CB_RESETCONTENT, 0, 0);
        for (const auto &choice : choices)
            SendMessageW(monitor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label.c_str()));
        SendMessageW(monitor, CB_SETCURSEL, index, 0);
    }

    void update_mode() {
        const auto selected = static_cast<FallbackMode>(SendMessageW(mode, CB_GETCURSEL, 0, 0));
        EnableWindow(monitor_label, selected == FallbackMode::monitor);
        EnableWindow(monitor, selected == FallbackMode::monitor);
        const auto text = selected == FallbackMode::monitor
                              ? L"Clones the entire selected desktop. Keep Stream Output on a different "
                                L"display to prevent a video feedback loop."
                          : selected == FallbackMode::hide
                              ? L"Stream Output reappears automatically when game capture resumes."
                              : L"Stream Output shows the waiting screen until game capture resumes.";
        SetWindowTextW(mode_help, text);
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, msg, w, l);
        LRESULT result{};
        if (self->theme->message(h, msg, w, l, result))
            return result;
        if (msg == WM_DISPLAYCHANGE && self->monitor) {
            const auto index = size_t(SendMessageW(self->monitor, CB_GETCURSEL, 0, 0));
            if (index < self->choices.size())
                self->refresh_monitors(self->choices[index].name);
        }
        if (msg == WM_COMMAND) {
            switch (LOWORD(w)) {
            case mode_id:
                if (HIWORD(w) == CBN_SELCHANGE)
                    self->update_mode();
                return 0;
            case IDOK: {
                const auto mode = SendMessageW(self->mode, CB_GETCURSEL, 0, 0);
                const auto index = size_t(SendMessageW(self->monitor, CB_GETCURSEL, 0, 0));
                if (mode == CB_ERR || index >= self->choices.size())
                    return 0;
                OutputFallbackSettings value{static_cast<FallbackMode>(mode),
                                             SendMessageW(self->cockpit, BM_GETCHECK, 0, 0) == BST_CHECKED,
                                             self->choices[index].name};
                ShowWindow(h, SW_HIDE);
                self->applied(std::move(value));
                return 0;
            }
            case IDCANCEL:
                ShowWindow(h, SW_HIDE);
                return 0;
            }
        }
        if (msg == WM_CLOSE) {
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (msg == WM_NCDESTROY)
            self->window = nullptr;
        return DefWindowProcW(h, msg, w, l);
    }

    void create(HWND owner) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SteadyXROutputFallback";
        RegisterClassW(&wc);
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT r{0, 0, 524, 356}, parent{};
        GetWindowRect(owner, &parent);
        AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, GetDpiForWindow(owner));
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"SteadyXR - Stream Output fallback",
                                 style, parent.left + 40, parent.top + 40, r.right - r.left, r.bottom - r.top,
                                 owner, nullptr, wc.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        auto child = [&](const wchar_t *kind, const wchar_t *text, DWORD flags, int id, int x, int y,
                         int width, int height) {
            auto h =
                CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | theme->style(kind, flags), x, y, width,
                                height, window, reinterpret_cast<HMENU>(INT_PTR(id)), wc.hInstance, nullptr);
            theme->control(h, kind);
            return h;
        };
        constexpr DWORD combo = WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL;
        child(L"STATIC", L"When OpenXR frames are unavailable", 0, 0, 28, 24, 468, 24);
        mode = child(L"COMBOBOX", L"", combo, mode_id, 28, 51, 468, 130);
        for (auto label : {L"Show the waiting screen", L"Hide Stream Output", L"Clone a monitor"})
            SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        monitor_label = child(L"STATIC", L"Monitor to clone", 0, 0, 28, 91, 468, 23);
        monitor = child(L"COMBOBOX", L"", combo, monitor_id, 28, 116, 468, 200);
        mode_help = child(L"STATIC", L"", 0, 0, 28, 153, 468, 48);
        cockpit = child(L"BUTTON", L"Use fallback outside the car in iRacing and Le Mans Ultimate",
                        WS_TABSTOP | BS_AUTOCHECKBOX, cockpit_id, 28, 209, 468, 28);
        child(L"STATIC",
              L"With this enabled, those games must confirm you are in the car. "
              L"Other games use OpenXR frame availability.",
              0, 0, 28, 243, 468, 50);
        child(L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, 280, 300, 102, 34);
        auto apply = child(L"BUTTON", L"Apply", WS_TABSTOP, IDOK, 394, 300, 102, 34);
        SetPropW(apply, L"SteadyXRPrimary", reinterpret_cast<HANDLE>(1));
        theme->scale_window(window);
        MONITORINFO display{sizeof(display)};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &display))
            theme->fit_window(window, display.rcWork);
    }
};

OutputFallbackDialog::OutputFallbackDialog() : impl_(std::make_unique<Impl>()) {}
OutputFallbackDialog::~OutputFallbackDialog() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void OutputFallbackDialog::open(HWND owner, Theme &theme, const OutputFallbackSettings &settings,
                                std::function<void(OutputFallbackSettings)> applied) {
    auto &p = *impl_;
    p.theme = &theme;
    p.applied = std::move(applied);
    if (!p.window)
        p.create(owner);
    if (!p.window)
        return;
    if (!IsWindowVisible(p.window)) {
        p.refresh_monitors(settings.monitor_name);
        SendMessageW(p.mode, CB_SETCURSEL, WPARAM(settings.mode), 0);
        SendMessageW(p.cockpit, BM_SETCHECK, settings.cockpit_only ? BST_CHECKED : BST_UNCHECKED, 0);
        p.update_mode();
        ShowWindow(p.window, SW_SHOW);
        SetFocus(p.mode);
    }
    SetForegroundWindow(p.window);
}
bool OutputFallbackDialog::route(MSG &msg) {
    auto w = impl_->window;
    return w && IsWindowVisible(w) && (msg.hwnd == w || IsChild(w, msg.hwnd)) && IsDialogMessageW(w, &msg);
}
} // namespace vrst::preview
