#define UNICODE
#define _UNICODE
#include "spectator_controls.hpp"
#include "director_settings_io.hpp"
#include "spectator_settings.hpp"
#include "visual_settings_io.hpp"
#include <commctrl.h>
#include <cwchar>
#include <vector>

namespace vrst::preview {
struct SpectatorControls::Impl {
    HWND parent{}, window{}, status{}, compare{}, director_status{};
    Theme *theme{};
    Options *options{};
    Renderer *renderer{};
    std::function<void()> changed, learn;
    std::vector<HWND> pages[5];
    std::wstring last_status, last_director_status;
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
        else
            SetPropW(h, L"SteadyXRSurface", reinterpret_cast<HANDLE>(1));
        return h;
    }
    HWND note(const wchar_t *text, int x, int y, int width, int height, int group = -1) {
        const auto h = child(L"STATIC", text, 0, x, y, width, height, 0, group);
        SetPropW(h, L"SteadyXRMuted", reinterpret_cast<HANDLE>(1));
        return h;
    }
    void number(const wchar_t *label, double lo, double hi, int index, int id, int group, int start = 294) {
        const int x = 44 + (index % 2) * 474, y = start + (index / 2) * 50;
        child(L"STATIC", label, 0, x, y, 300, 22, 0, group);
        child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x + 308, y + 2, 110, 30, id, group);
        wchar_t range[64]{};
        swprintf(range, 64, L"Range: %g to %g", lo, hi);
        note(range, x, y + 22, 300, 22, group);
    }
    void show_page() {
        const auto focus = GetFocus();
        for (unsigned p = 0; p < std::size(pages); ++p)
            if (p != selected && focus &&
                std::find(pages[p].begin(), pages[p].end(), focus) != pages[p].end())
                SetFocus(GetDlgItem(window, page_id + selected));
        for (unsigned p = 0; p < std::size(pages); ++p) {
            for (auto h : pages[p])
                ShowWindow(h, p == selected ? SW_SHOW : SW_HIDE);
            const auto tab = GetDlgItem(window, page_id + p);
            SendMessageW(tab, BM_SETCHECK, p == selected ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(tab, nullptr, FALSE);
        }
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
        for (unsigned i = 0; i < 3u; ++i)
            CheckDlgButton(window, camera_bool_id + i,
                           options->spectator.*spectator_bool_fields[i].member ? BST_CHECKED : BST_UNCHECKED);
        for (unsigned i = 0; i < 8u; ++i)
            text_number(camera_number_id + i, options->spectator.*spectator_number_fields[i].member);
        for (unsigned i = 0; i < std::size(visual_bool_fields); ++i)
            CheckDlgButton(window, visual_bool_id + i,
                           options->visual.*visual_bool_fields[i].member ? BST_CHECKED : BST_UNCHECKED);
        for (unsigned i = 0; i < std::size(visual_number_fields); ++i)
            text_number(visual_number_id + i, options->visual.*visual_number_fields[i].member);
        for (unsigned i = 0; i < std::size(visual_region_fields); ++i)
            text_number(region_id + i, options->visual.metering_region.*visual_region_fields[i].member);
        for (unsigned i = 0; i < std::size(director_bool_fields); ++i)
            CheckDlgButton(window, director_bool_id + i,
                           options->director.*director_bool_fields[i].member ? BST_CHECKED : BST_UNCHECKED);
        for (unsigned i = 0; i < std::size(director_number_fields); ++i)
            text_number(director_number_id + i, options->director.*director_number_fields[i].member);
        SendMessageW(GetDlgItem(window, telemetry_id), CB_SETCURSEL, options->director.telemetry_game, 0);
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
        if (text != last_status) {
            SetWindowTextW(status, text.c_str());
            last_status = text;
        }
        EnableWindow(GetDlgItem(window, learn_id),
                     !options->spectator.bypass && !renderer->calibration_active());
        EnableWindow(GetDlgItem(window, cancel_id), renderer->calibration_active());
        EnableWindow(GetDlgItem(window, clear_id), s.zone_valid);
        const auto d = renderer->director_status();
        wchar_t detail[512]{};
        swprintf(detail, std::size(detail),
                 L"Lookahead %ls: requested %.0f frames; actual %u frames / %.1f ms; %.1f MiB GPU.\r\n"
                 L"Automatic crop %.1f%%; speed crop %.1f%%.",
                 options->director.lookahead_enabled ? L"on" : L"off", options->director.lookahead_frames,
                 d.buffered_frames, d.delay_ms, double(d.buffered_bytes) / (1024 * 1024), d.auto_crop_percent,
                 d.speed_crop_percent);
        std::wstring director_text = detail;
        if (options->spectator.bypass)
            director_text += L"\r\nSpectator camera bypassed.";
        else if (!options->director.speed_fov_enabled)
            director_text += L"\r\nSpeed framing off.";
        else if (d.speed_available) {
            swprintf(detail, std::size(detail), L"\r\n%ls: %.0f km/h.",
                     director_telemetry_labels[std::min(d.telemetry_game, 2u)], d.speed_kph);
            director_text += detail;
        } else
            director_text += L"\r\nNo live speed; framing returns to neutral.";
        if (d.buffer_limited)
            director_text += L" Lookahead limited by the 256 MiB buffer cap.";
        if (director_text != last_director_status) {
            SetWindowTextW(director_status, director_text.c_str());
            last_director_status = std::move(director_text);
        }
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
        if (id >= page_id && id < page_id + int(std::size(pages))) {
            selected = unsigned(id - page_id);
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
        else if (id >= camera_bool_id && id < camera_bool_id + int(3u))
            options->spectator.*spectator_bool_fields[id - camera_bool_id].member =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id >= visual_bool_id && id < visual_bool_id + int(std::size(visual_bool_fields)))
            options->visual.*visual_bool_fields[id - visual_bool_id].member =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id >= director_bool_id && id < director_bool_id + int(std::size(director_bool_fields)))
            options->director.*director_bool_fields[id - director_bool_id].member =
                IsDlgButtonChecked(window, id) == BST_CHECKED;
        else if (id == telemetry_id) {
            if (notification != CBN_SELCHANGE)
                return;
            options->director.telemetry_game = director_telemetry_selection(
                double(SendMessageW(GetDlgItem(window, telemetry_id), CB_GETCURSEL, 0, 0)));
        } else if (id >= camera_number_id && id < camera_number_id + int(8u)) {
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
        } else if (id >= director_number_id &&
                   id < director_number_id + int(std::size(director_number_fields))) {
            if (notification != EN_KILLFOCUS)
                return;
            double value{};
            if (read_number(id, value))
                options->director.*director_number_fields[id - director_number_id].member = value;
        } else if (id >= region_id && id < region_id + int(std::size(visual_region_fields))) {
            if (notification != EN_KILLFOCUS)
                return;
            double value{};
            if (read_number(id, value))
                options->visual.metering_region.*visual_region_fields[id - region_id].member = float(value);
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
        options->spectator.natural_enabled = false;
        options->director = sanitize_director_settings(options->director);
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
        window =
            CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"SteadyXR - Gaze & effects", style, 120,
                            80, r.right - r.left, r.bottom - r.top, parent, nullptr, cls.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        const auto heading = child(L"STATIC", L"Gaze & effects", 0, 28, 22, 580, 32, 0);
        theme->control(heading, L"STATIC", true);
        note(L"Shape the view your audience sees. Every enhancement is optional.", 28, 60, 600, 26);
        child(L"BUTTON", L"Bypass spectator camera", BS_AUTOCHECKBOX, 650, 20, 324, 28, camera_bool_id);
        child(L"BUTTON", L"Bypass visual effects", BS_AUTOCHECKBOX, 650, 60, 324, 28, visual_bool_id);
        const wchar_t *tabs[]{L"Gaze & chat", L"Exposure", L"Bloom", L"Color", L"Director"};
        const wchar_t *titles[]{L"Follow your focus", L"Adapt to changing light",
                                L"Give bright areas a soft glow", L"Create your look",
                                L"Camera timing & framing"};
        const wchar_t *descriptions[]{
            L"Requires OpenXR eye tracking. Chat exclusion ignores gaze toward chat while head following "
            L"continues.",
            L"Leave a tunnel: the view briefly brightens, then settles. Enter shade: it gradually brightens "
            L"again.",
            L"Bloom follows bright highlights continuously; it does not trigger a flash when lighting "
            L"changes.",
            L"A consistent color grade for the viewer. These adjustments do not react to brightness "
            L"changes.",
            L"Delay for smoother camera motion, cover exposed edges, or vary framing with car speed."};
        for (int p = 0; p < int(std::size(pages)); ++p) {
            const auto tab =
                child(L"BUTTON", tabs[p], BS_AUTORADIOBUTTON | BS_PUSHLIKE | (p == 0 ? WS_GROUP : 0),
                      28 + p * 190, 102, 184, 42, page_id + p);
            SetPropW(tab, L"SteadyXRTab", reinterpret_cast<HANDLE>(1));
            const auto title = child(L"STATIC", titles[p], 0, 44, 184, 912, 28, 0, p);
            theme->control(title, L"STATIC", true);
            note(descriptions[p], 44, 218, 912, 30, p);
        }
        child(L"BUTTON", L"Enable gaze-driven framing", BS_AUTOCHECKBOX | WS_GROUP, 44, 256, 440, 28,
              camera_bool_id + 1, 0);
        child(L"BUTTON", L"Ignore gaze toward learned chat zone", BS_AUTOCHECKBOX, 518, 256, 440, 28,
              camera_bool_id + 2, 0);
        for (unsigned i = 0; i < 8; ++i) {
            const auto &f = spectator_number_fields[i];
            number(f.label, f.minimum, f.maximum, i, camera_number_id + i, 0);
        }
        status = child(L"STATIC", L"", 0, 44, 500, 912, 62, status_id, 0);
        note(L"Select Learn, then look at chat and hold steady through the countdown. Shortcut: Ctrl+Alt+C.",
             44, 567, 912, 26, 0);
        child(L"BUTTON", L"Look at chat now / Learn", BS_PUSHBUTTON, 44, 600, 270, 34, learn_id, 0);
        child(L"BUTTON", L"Cancel learning", BS_PUSHBUTTON, 330, 600, 220, 34, cancel_id, 0);
        child(L"BUTTON", L"Clear chat zone", BS_PUSHBUTTON, 566, 600, 220, 34, clear_id, 0);

        child(L"BUTTON", L"Enable manual exposure", BS_AUTOCHECKBOX, 44, 256, 440, 28, visual_bool_id + 1, 1);
        child(L"BUTTON", L"Enable light adaptation", BS_AUTOCHECKBOX, 518, 256, 440, 28, visual_bool_id + 2,
              1);
        for (unsigned i = 0; i < std::size(visual_number_fields); ++i) {
            const auto &f = visual_number_fields[i];
            const int group = i < 8 ? 1 : i < 12 ? 2 : 3;
            const int index = i < 8 ? i : i < 12 ? i - 8 : i - 12;
            number(f.label, f.minimum, f.maximum, index, visual_number_id + i, group);
        }
        note(L"Metering region · 0 = left/top, 1 = right/bottom. Use the road area to reduce dashboard "
             L"influence.",
             44, 504, 912, 26, 1);
        for (unsigned i = 0; i < std::size(visual_region_fields); ++i)
            number(visual_region_fields[i].label, 0, 1, i, region_id + i, 1, 536);

        child(L"BUTTON", L"Enable bloom", BS_AUTOCHECKBOX, 44, 256, 912, 28, visual_bool_id + 3, 2);
        note(L"Threshold selects the highlights that glow. Soft knee blends the transition.\r\n"
             L"Strength controls how much glow is added; radius controls how far it spreads.\r\n\r\n"
             L"Start subtle: too much bloom can hide track detail and wash out the picture.",
             44, 438, 912, 116, 2);
        child(L"BUTTON", L"Enable color adjustments", BS_AUTOCHECKBOX, 44, 256, 912, 28, visual_bool_id + 4,
              3);
        note(L"Temperature shifts warm / cool; tint shifts green / magenta.\r\n"
             L"Contrast and saturation shape the overall look. Shadows and highlights tune the tonal "
             L"balance.\r\n\r\n"
             L"Use A/B below to compare the grade with the same camera framing.",
             44, 488, 912, 112, 3);

        for (unsigned i = 0; i < std::size(director_bool_fields); ++i)
            child(L"BUTTON", director_bool_fields[i].label, BS_AUTOCHECKBOX | (i == 0 ? WS_GROUP : 0),
                  44 + int(i) * 306, 256, 300, 28, director_bool_id + i, 4);
        for (unsigned i = 0; i < std::size(director_number_fields); ++i) {
            const auto &f = director_number_fields[i];
            number(f.label, f.minimum, f.maximum, i, director_number_id + i, 4);
        }
        child(L"STATIC", L"Speed telemetry", 0, 44, 456, 200, 26, 0, 4);
        const auto telemetry =
            child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 250, 450, 260, 160, telemetry_id, 4);
        for (const auto label : director_telemetry_labels)
            SendMessageW(telemetry, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        note(L"Low speed adds crop; high speed restores normal framing.", 530, 450, 426, 44, 4);
        director_status = child(L"STATIC", L"", 0, 44, 504, 912, 68, director_status_id, 4);
        note(L"OpenXR lookahead delays video: add the shown delay to the OBS audio sync offset.\r\n"
             L"Window capture supports automatic crop and speed framing; lookahead needs OpenXR.",
             44, 584, 912, 48, 4);

        child(L"BUTTON", L"Apply filmic visual preset", BS_PUSHBUTTON, 28, 658, 280, 36, filmic_id);
        child(L"BUTTON", L"Reset visual effects to off", BS_PUSHBUTTON, 324, 658, 280, 36, neutral_id);
        note(L"Apply visuals here OR on the stream PC.\r\nValues apply when you leave a field.", 626, 656,
             348, 46);
        compare = child(L"BUTTON", L"A/B compares visual effects only (same camera framing)", BS_AUTOCHECKBOX,
                        28, 712, 944, 26, compare_scope_id);
        refresh();
        show_page();
        theme->scale_window(window);
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            const auto dc = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            self->theme->surface(h, dc, {20, 164, 980, 644});
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
