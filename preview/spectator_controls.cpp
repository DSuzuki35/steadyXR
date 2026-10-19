#define UNICODE
#define _UNICODE
#include "spectator_controls.hpp"
#include "spectator_settings.hpp"
#include "visual_settings_io.hpp"
#include <commctrl.h>
#include <cwchar>
#include <vector>

namespace vrst::preview {
struct SpectatorControls::Impl {
    HWND parent{}, window{}, page{}, status{}, compare{};
    Theme *theme{};
    Options *options{};
    Renderer *renderer{};
    std::function<void()> changed, learn;
    std::vector<HWND> pages[2];
    unsigned selected{};
    bool loading{};
    HWND child(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int width, int height,
               int id, int group = -1) {
        if (wcscmp(kind, L"STATIC"))
            style |= WS_TABSTOP;
        const auto h = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | theme->style(kind, style), x, y,
                                       width, height, window, reinterpret_cast<HMENU>(INT_PTR(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        theme->control(h, kind);
        if (group >= 0)
            pages[group].push_back(h);
        return h;
    }
    void number(const wchar_t *label, double lo, double hi, int index, int id, int group, int start = 190) {
        const int x = 20 + (index % 2) * 490, y = start + (index / 2) * 38;
        child(L"STATIC", label, 0, x, y + 3, 280, 26, 0, group);
        child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x + 290, y, 110, 27, id, group);
        wchar_t range[64]{};
        swprintf(range, 64, L"%g-%g", lo, hi);
        child(L"STATIC", range, 0, x + 405, y + 4, 78, 24, 0, group);
    }
    void show_page() {
        for (unsigned p = 0; p < 2; ++p)
            for (auto h : pages[p])
                ShowWindow(h, p == selected ? SW_SHOW : SW_HIDE);
    }
    void text_number(int id, double value) {
        wchar_t text[48]{};
        swprintf(text, 48, L"%.6g", value);
        SetDlgItemTextW(window, id, text);
    }
    void refresh() {
        if (!window)
            return;
        loading = true;
        for (unsigned i = 0; i < std::size(spectator_bool_fields); ++i)
            CheckDlgButton(window, camera_bool_id + i,
                           options->spectator.*spectator_bool_fields[i].member ? BST_CHECKED : BST_UNCHECKED);
        for (unsigned i = 0; i < std::size(spectator_number_fields); ++i)
            text_number(camera_number_id + i, options->spectator.*spectator_number_fields[i].member);
        for (unsigned i = 0; i < std::size(visual_bool_fields); ++i)
            CheckDlgButton(window, visual_bool_id + i,
                           options->visual.*visual_bool_fields[i].member ? BST_CHECKED : BST_UNCHECKED);
        for (unsigned i = 0; i < std::size(visual_number_fields); ++i)
            text_number(visual_number_id + i, options->visual.*visual_number_fields[i].member);
        for (unsigned i = 0; i < std::size(visual_region_fields); ++i)
            text_number(region_id + i, options->visual.metering_region.*visual_region_fields[i].member);
        CheckDlgButton(window, compare_scope_id, options->compare_effects ? BST_CHECKED : BST_UNCHECKED);
        loading = false;
        update_status();
    }
    void update_status() {
        if (!window)
            return;
        const auto s = renderer->spectator_status();
        const wchar_t *capabilities[]{L"Gaze disabled / no OpenXR eye data",
                                      L"Eye tracking unsupported by runtime",
                                      L"Waiting for game action set attachment",
                                      L"Eye tracking inactive",
                                      L"Eye tracking available",
                                      L"Eye tracking has unknown sample time"};
        std::wstring text = capabilities[std::min(renderer->gaze_capability(), 5u)];
        const wchar_t *states[]{L"Gaze framing off",    L"Gaze currently untracked",
                                L"Gaze sample stale",   L"Waiting for a steady fixation",
                                L"Gaze framing active", L"Chat gaze excluded; head following continues"};
        text += L" — ";
        text += options->spectator.bypass          ? L"Spectator enhancements bypassed"
                : !options->spectator.gaze_enabled ? states[0]
                                                   : states[static_cast<unsigned>(s.gaze_state)];
        if (s.calibration_phase == ChatCalibrationPhase::countdown) {
            wchar_t count[64]{};
            swprintf(count, 64, L"\r\nLook at chat; hold steady. Starting in %.1f s.", s.countdown_seconds);
            text += count;
        } else if (s.calibration_phase == ChatCalibrationPhase::acquiring) {
            text += L"\r\nLearning a stable direction: " +
                    std::to_wstring(int(s.calibration_progress * 100)) + L"% (" +
                    std::to_wstring(s.calibration_samples) + L" samples). Keep looking at chat.";
        } else if (s.calibration_phase == ChatCalibrationPhase::failed) {
            text += s.calibration_failure == ChatCalibrationFailure::unstable
                        ? L"\r\nLearning failed: gaze moved. Try again while holding steady."
                    : s.calibration_failure == ChatCalibrationFailure::reference_changed
                        ? L"\r\nLearning reset: tracking reference changed. Learn again."
                        : L"\r\nLearning failed: insufficient valid eye samples. Check eye tracking.";
        } else
            text += s.zone_valid ? L"\r\nChat zone learned for this session. "
                                 : L"\r\nNo learned chat zone in this session. ";
        if (s.zone_valid && (!options->spectator.gaze_enabled || options->spectator.bypass) &&
            !renderer->calibration_active())
            text += L"Chat coverage unavailable while gaze acquisition is off.";
        else if (s.in_chat_zone && !options->spectator.chat_enabled)
            text += L"Looking at learned chat zone; exclusion is off.";
        SetWindowTextW(status, text.c_str());
        EnableWindow(GetDlgItem(window, cancel_id), renderer->calibration_active());
    }
    bool read_number(int id, double &value) {
        wchar_t text[64]{}, *end{};
        GetDlgItemTextW(window, id, text, 64);
        value = wcstod(text, &end);
        if (end == text || !std::isfinite(value))
            return false;
        while (*end && iswspace(*end))
            ++end;
        return !*end;
    }
    void command(int id, int notification) {
        if (loading)
            return;
        if (id == page_id && notification == CBN_SELCHANGE) {
            selected = unsigned(SendMessageW(page, CB_GETCURSEL, 0, 0));
            show_page();
            return;
        }
        if (id == learn_id) {
            learn();
            update_status();
            return;
        }
        if (id == cancel_id) {
            renderer->cancel_chat();
            changed();
            update_status();
            return;
        }
        if (id == clear_id) {
            renderer->clear_chat();
            changed();
            update_status();
            return;
        }
        if (id == compare_scope_id)
            options->compare_effects = IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id >= camera_bool_id && id < camera_bool_id + int(std::size(spectator_bool_fields)))
            options->spectator.*spectator_bool_fields[id - camera_bool_id].member =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id >= visual_bool_id && id < visual_bool_id + int(std::size(visual_bool_fields)))
            options->visual.*visual_bool_fields[id - visual_bool_id].member =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id >= camera_number_id && id < camera_number_id + int(std::size(spectator_number_fields))) {
            if (notification != EN_KILLFOCUS)
                return;
            double value{};
            if (read_number(id, value))
                options->spectator.*spectator_number_fields[id - camera_number_id].member = value;
        } else if (id >= visual_number_id && id < visual_number_id + int(std::size(visual_number_fields))) {
            if (notification != EN_KILLFOCUS)
                return;
            double value{};
            if (read_number(id, value))
                options->visual.*visual_number_fields[id - visual_number_id].member = float(value);
        } else if (id >= region_id && id < region_id + int(std::size(visual_region_fields))) {
            if (notification != EN_KILLFOCUS)
                return;
            double value{};
            if (read_number(id, value))
                options->visual.metering_region.*visual_region_fields[id - region_id].member = float(value);
        } else if (id == natural_id || id == cinematic_id) {
            auto &s = options->spectator;
            const auto natural = SpectatorSettings{};
            s.bypass = false;
            s.natural_enabled = true;
            s.natural_suppression = id == cinematic_id ? 85 : natural.natural_suppression;
            s.natural_follow = id == cinematic_id ? 45 : natural.natural_follow;
            s.natural_settling_ms = id == cinematic_id ? 300 : natural.natural_settling_ms;
            s.natural_max_correction_degrees = natural.natural_max_correction_degrees;
            s.natural_yaw = natural.natural_yaw;
            s.natural_pitch = natural.natural_pitch;
            s.natural_roll = natural.natural_roll;
        } else if (id == filmic_id) {
            auto v = VisualSettings{};
            v.color = v.bloom = true;
            v.contrast = 1.06f;
            v.saturation = .95f;
            v.bloom_strength = .06f;
            v.bloom_threshold = 1;
            options->visual = v;
        } else if (id == neutral_id)
            options->visual = VisualSettings{};
        else
            return;
        options->spectator = sanitize_spectator_settings(options->spectator);
        options->visual = options->visual.bounded();
        refresh();
        changed();
    }
    void create() {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"SteadyXRSpectatorControls";
        cls.lpfnWndProc = proc;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        RECT r{0, 0, 1000, 750};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName,
                                 L"SteadyXR - Spectator camera / visual effects", style, 120, 80,
                                 r.right - r.left, r.bottom - r.top, parent, nullptr, cls.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        child(L"STATIC", L"SPECTATOR CAMERA / VISUAL EFFECTS", 0, 20, 20, 650, 26, 0);
        page = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 710, 16, 270, 100, page_id);
        SendMessageW(page, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Spectator camera"));
        SendMessageW(page, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Visual effects"));
        SendMessageW(page, CB_SETCURSEL, 0, 0);
        for (unsigned i = 0; i < std::size(spectator_bool_fields); ++i)
            child(L"BUTTON", spectator_bool_fields[i].label, BS_AUTOCHECKBOX, 20, 60 + i * 30, 480, 26,
                  camera_bool_id + i, 0);
        child(L"STATIC",
              L"OpenXR eye tracking required (no eye data in window mode).\r\nLearn: look at chat and hold "
              L"steady through the countdown.\r\nCtrl+Alt+C learns; alternate Ctrl+Alt+Shift+C in "
              L"Hotkeys.\r\nLearning works with framing off; bypass stops eye requests.",
              0, 510, 62, 470, 110, 0, 0);
        for (unsigned i = 0; i < std::size(spectator_number_fields); ++i) {
            const auto &f = spectator_number_fields[i];
            number(f.label, f.minimum, f.maximum, i, camera_number_id + i, 0);
        }
        status = child(L"STATIC", L"", 0, 20, 512, 960, 66, status_id, 0);
        child(L"BUTTON", L"Learn chat zone", BS_PUSHBUTTON, 20, 590, 210, 34, learn_id, 0);
        child(L"BUTTON", L"Cancel learning", BS_PUSHBUTTON, 245, 590, 210, 34, cancel_id, 0);
        child(L"BUTTON", L"Clear chat zone", BS_PUSHBUTTON, 470, 590, 210, 34, clear_id, 0);
        child(L"BUTTON", L"Natural camera preset", BS_PUSHBUTTON, 20, 644, 310, 34, natural_id, 0);
        child(L"BUTTON", L"Cinematic camera preset", BS_PUSHBUTTON, 350, 644, 330, 34, cinematic_id, 0);
        for (unsigned i = 0; i < std::size(visual_bool_fields); ++i)
            child(L"BUTTON", visual_bool_fields[i].label, BS_AUTOCHECKBOX, 20 + (i % 2) * 490,
                  60 + (i / 2) * 30, 470, 26, visual_bool_id + i, 1);
        for (unsigned i = 0; i < std::size(visual_number_fields); ++i) {
            const auto &f = visual_number_fields[i];
            number(f.label, f.minimum, f.maximum, i, visual_number_id + i, 1, 165);
        }
        for (unsigned i = 0; i < std::size(visual_region_fields); ++i)
            number(visual_region_fields[i].label, 0, 1, i, region_id + i, 1, 530);
        child(L"STATIC",
              L"Apply effects in one location: preview/direct source OR the stream PC effects "
              L"filter.\r\nChanges apply on leaving each field; input outside its range is bounded.",
              0, 20, 610, 960, 44, 0, 1);
        child(L"BUTTON", L"Filmic visual preset", BS_PUSHBUTTON, 20, 665, 310, 34, filmic_id, 1);
        child(L"BUTTON", L"Reset visual effects to off", BS_PUSHBUTTON, 350, 665, 330, 34, neutral_id, 1);
        compare = child(L"BUTTON", L"A/B compares visual effects only (same camera framing)", BS_AUTOCHECKBOX,
                        20, 710, 960, 26, compare_scope_id);
        refresh();
        show_page();
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        // Theme supplies all native control painting; this panel has no main-window header.
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            const auto dc = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            RECT r{};
            GetClientRect(h, &r);
            SetDCBrushColor(dc, RGB(24, 31, 42));
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (m == WM_PAINT)
                EndPaint(h, &ps);
            return 0;
        }
        LRESULT result{};
        if (self->theme->message(h, m, w, l, result))
            return result;
        if (m == WM_COMMAND) {
            self->command(LOWORD(w), HIWORD(w));
            return 0;
        }
        if (m == WM_TIMER) {
            self->update_status();
            return 0;
        }
        if (m == WM_CLOSE) {
            ShowWindow(h, SW_HIDE);
            KillTimer(h, 1);
            return 0;
        }
        if (m == WM_DESTROY) {
            self->window = nullptr;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};
SpectatorControls::SpectatorControls() : impl_(std::make_unique<Impl>()) {}
SpectatorControls::~SpectatorControls() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void SpectatorControls::initialize(HWND parent, Theme &theme, Options &options, Renderer &renderer,
                                   std::function<void()> changed, std::function<void()> learn) {
    impl_->parent = parent;
    impl_->theme = &theme;
    impl_->options = &options;
    impl_->renderer = &renderer;
    impl_->changed = std::move(changed);
    impl_->learn = std::move(learn);
}
void SpectatorControls::open(bool visible) {
    if (!impl_->window)
        impl_->create();
    impl_->refresh();
    if (visible) {
        ShowWindow(impl_->window, SW_SHOW);
        SetTimer(impl_->window, 1, 100, nullptr);
    }
}
void SpectatorControls::refresh() {
    impl_->refresh();
}
HWND SpectatorControls::handle() const {
    return impl_->window;
}
bool SpectatorControls::route(MSG &msg) {
    return impl_->window && IsWindowVisible(impl_->window) && IsDialogMessageW(impl_->window, &msg);
}
} // namespace vrst::preview
